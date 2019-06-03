#include <algorithm>
#include <utils.h>
#include <TimeUtils.h>
#include "OpXfer.h"
#include "../UI/Activities/ConfirmXfer.h"
#include "../UI/Activities/ConfirmOverwrite.h"
#include "../UI/Activities/WhatOnError.h"
#include "../UI/Activities/ComplexOperationProgress.h"
#include "../lng.h"

#define BUFFER_SIZE_GRANULARITY   0x8000
#define BUFFER_SIZE_LIMIT         0x1000000
#define BUFFER_SIZE_INITIAL       (2 * BUFFER_SIZE_GRANULARITY)

#define EXTRA_NEEDED_MODE	(S_IRUSR | S_IWUSR)

OpXfer::OpXfer(int op_mode, std::shared_ptr<IHost> &base_host, const std::string &base_dir,
	std::shared_ptr<IHost> &dst_host, const std::string &dst_dir,
	struct PluginPanelItem *items, int items_count, XferKind kind, XferDirection direction)
	:
	OpBase(op_mode, base_host, base_dir),

	_dst_host(dst_host),
	_dst_dir(dst_dir),
	_kind(kind),
	_direction(direction),
	_io_buf(BUFFER_SIZE_INITIAL, BUFFER_SIZE_GRANULARITY, BUFFER_SIZE_LIMIT)
{
	_enumer = std::make_shared<Enumer>(_entries, _base_host, _base_dir, items, items_count, true, _state, _wea_state);
	_diffname_suffix = ".NetRocks@";
	_diffname_suffix+= TimeString(TSF_FOR_FILENAME);

	if (!IS_SILENT(_op_mode)) {
		_kind = ConfirmXfer(_kind, _direction).Ask(_default_xoa, _dst_dir);
		if (_kind == XK_NONE) {
			fprintf(stderr, "NetRocks::Xfer: cancel\n");
			throw AbortError();
		}
	} else {
		_default_xoa = XOA_OVERWRITE_IF_NEWER_OTHERWISE_ASK;
	}

	if (_kind == XK_RENAME) {
		SetNotifyTitle(MNotificationRename);
	} else {
		if (_dst_dir.empty()) {
			_dst_dir = "./";
		} else if (_dst_dir[_dst_dir.size() - 1] != '/')
			_dst_dir+= '/';

		if (direction == XD_UPLOAD) {
			SetNotifyTitle(MNotificationUpload);
		} else if (direction == XD_DOWNLOAD) {
			SetNotifyTitle(MNotificationDownload);
		} else if (direction == XD_CROSSLOAD) {
			SetNotifyTitle(MNotificationCrossload);
		} else {
			throw std::runtime_error("Wrong direction");
		}
	}

	if (!StartThread()) {
		throw std::runtime_error("Cannot start thread");
	}

	if (!WaitThreadBeforeShowProgress()) {
		XferProgress p(_kind, _direction, _dst_dir, _state);
		p.Show(false);
	}
}

OpXfer::~OpXfer()
{
//	sleep(10);
	WaitThread();
}

BackgroundTaskStatus OpXfer::GetStatus()
{
	{
		std::lock_guard<std::mutex> locker(_state.mtx);
		if (!_state.finished)
			return _state.paused ? BTS_PAUSED : BTS_ACTIVE;
	}

	WaitThread();
	return (GetThreadResult() == nullptr) ? BTS_COMPLETE : BTS_ABORTED;
}

std::string OpXfer::GetInformation()
{
	std::string out;
	if (!WaitThread( 0 )) {
		ProgressStateStats stats;
		{
			std::lock_guard<std::mutex> locker(_state.mtx);
			stats = _state.stats;
		}

		if (stats.all_total != 0) {
			out+= StrPrintf("%u%% ", (unsigned int)(stats.all_complete * 100ll / stats.all_total));
		} else {
			out+= "... ";
		}
	}

	out+= _base_host->SiteName();
	out+= " -> ";
	out+= _dst_host->SiteName();
	return out;
}

void OpXfer::Show()
{
	XferProgress p(_kind, _direction, _dst_dir, _state);
	p.Show(true);
}

void OpXfer::Abort()
{
	{
		std::lock_guard<std::mutex> locker(_state.mtx);
		if (_state.finished)
			return;
		_state.aborting = true;
	}

	_dst_host->Abort();
	_base_host->Abort();
}

void OpXfer::Process()
{
	if (_kind == XK_RENAME) {
		if (_enumer) {
			Rename(_enumer->Items());
			_enumer.reset();
		}
	} else {
		if (_enumer) {
			_enumer->Scan();
			_enumer.reset();

			std::lock_guard<std::mutex> locker(_state.mtx);
			_state.stats.total_start = TimeMSNow();
			_state.stats.total_paused = std::chrono::milliseconds::zero();
		}
		Transfer();
	}
}

void OpXfer::Rename(const std::set<std::string> &items)
{
	if (_dst_dir == "*")
		return;

	std::string new_path;
	size_t star = _dst_dir.find("*"); // its actually name or wildcard

	for (const auto &original_path : items) {
			const std::string &original_name = original_path.substr(_base_dir.size());
			new_path = _base_dir;

			if (star == 0) {// *.txt: <foo.doc -> foo.txt>   <bar -> bar.txt>
				size_t p = original_name.rfind(_dst_dir[star + 1]);
				if (p != std::string::npos) {
					new_path+= original_name.substr(0, p);
				} else {
					new_path+= original_name;
					}
				new_path+= _dst_dir.substr(star + 1);

			} else if (star != std::string::npos) { // Foo*Bar: <Hello->FooHelloBar>
				new_path+= _dst_dir.substr(0, star);
				new_path+= original_name;
				new_path+= _dst_dir.substr(star + 1);
			} else {
				new_path+= _dst_dir;
			}

			WhatOnErrorWrap<WEK_RENAME>(_wea_state, _state, _base_host.get(), original_path + " -> " + new_path,
				[&] () mutable 
				{
					_base_host->Rename(original_path, new_path);
				}
		);
		
	}
}

void OpXfer::EnsureDstDirExists()
{
	try {
		_dst_host->GetMode(_dst_dir);
		return;
	} catch (std::exception &) { }

	for (size_t i = 1; i <= _dst_dir.size(); ++i) {
		if (i == _dst_dir.size() || _dst_dir[i] == '/') {
			const std::string &part_dir = _dst_dir.substr(0, i);
			try {
				_dst_host->GetMode(part_dir);
				continue;
			} catch (std::exception &) { }

			WhatOnErrorWrap<WEK_MAKEDIR>(_wea_state, _state, _dst_host.get(), part_dir,
				[&] () mutable 
				{
					_dst_host->DirectoryCreate(part_dir, 0751);
				}
			);
		}
	}
}

void OpXfer::Transfer()
{
	EnsureDstDirExists();

	std::string path_dst;
	for (auto &e : _entries) {
		const std::string &subpath = e.first.substr(_base_dir.size());
		path_dst = _dst_dir;
		path_dst+= subpath;
		{
			std::unique_lock<std::mutex> lock(_state.mtx);
			_state.path = subpath;
			_state.stats.file_complete = 0;
			_state.stats.file_total = S_ISDIR(e.second.mode) ? 0 : e.second.size;
			_state.stats.current_start = TimeMSNow();
			_state.stats.current_paused = std::chrono::milliseconds::zero();
		}

		FileInformation existing_file_info;
		bool existing = false;
		try {
			_dst_host->GetInformation(existing_file_info, path_dst);
			existing = true;
		} catch (std::exception &ex) { ; } // FIXME: distinguish unexistence of file from IO failure


		if (S_ISLNK(e.second.mode)) {
			if (existing || SymlinkCopy(e.first, path_dst)) {
				ProgressStateUpdate psu(_state);
				_state.stats.count_complete++;
				continue;
			}
			// if symlink copy failed then fallback to target's content copy
			WhatOnErrorWrap<WEK_QUERYINFO>(_wea_state, _state, _base_host.get(), e.first,
				[&] () mutable 
				{
					_base_host->GetInformation(e.second, e.first, true);
				}
			);
			if (!S_ISREG(e.second.mode) && !S_ISDIR(e.second.mode)) {
				// don't copy symlink's target if its nor file nor directory
				fprintf(stderr, "NetRocks: skipped symlink target with mode=0x%x - '%s' \n", e.second.mode, path_dst.c_str());
				ProgressStateUpdate psu(_state);
				_state.stats.count_complete++;
				_state.stats.count_skips++;
				continue;
			}

			if (S_ISREG(e.second.mode)) {
				// symlinks are not counted in all_total, need to add size for symlink's target if gonna file-copy it
				std::unique_lock<std::mutex> lock(_state.mtx);
				_state.stats.all_total+= e.second.size;
			}
		}

		if (S_ISDIR(e.second.mode)) {
			if (!existing) {
				DirectoryCopy(path_dst, e.second);
			}

		} else {
			if (existing) {
				auto xoa = _default_xoa;
				if (xoa == XOA_OVERWRITE_IF_NEWER_OTHERWISE_ASK) {
					xoa = (TimeSpecCompare(existing_file_info.modification_time, e.second.modification_time) < 0)
						? XOA_OVERWRITE : XOA_ASK;
				}

				if (xoa == XOA_ASK) {
					xoa = ConfirmOverwrite(_kind, _direction, path_dst, e.second.modification_time, e.second.size,
								existing_file_info.modification_time, existing_file_info.size).Ask(_default_xoa);
					if (xoa == XOA_CANCEL) {
						return;
					}
				}
				if (xoa == XOA_OVERWRITE_IF_NEWER) {
					xoa = (TimeSpecCompare(existing_file_info.modification_time, e.second.modification_time) < 0)
						? XOA_OVERWRITE : XOA_SKIP;
				}
				if (xoa == XOA_RESUME) {
					if (existing_file_info.size < e.second.size) {
						_state.stats.all_complete+= existing_file_info.size;
						_state.stats.file_complete = existing_file_info.size;
					} else {
						xoa = XOA_SKIP;
					}

				} else if (xoa == XOA_CREATE_DIFFERENT_NAME) {
					path_dst+= _diffname_suffix;
				}

				if (xoa == XOA_SKIP) {
					std::unique_lock<std::mutex> lock(_state.mtx);
					_state.stats.all_complete+= e.second.size;
					_state.stats.file_complete+= e.second.size;
					_state.stats.count_complete++;
					continue;
				}
			}
			if (FileCopyLoop(e.first, path_dst, e.second)) {
				CopyAttributes(path_dst, e.second);
				if (_kind == XK_MOVE) {
					WhatOnErrorWrap<WEK_REMOVE>(_wea_state, _state, _base_host.get(), e.first,
						[&] () mutable
						{
							_base_host->FileDelete(e.first);
						}
					);
				}
			}
		}

		ProgressStateUpdate psu(_state);
		_state.stats.count_complete++;
	}

	for (auto rev_i = _entries.rbegin(); rev_i != _entries.rend(); ++rev_i) {
		if (S_ISDIR(rev_i->second.mode)) {
			path_dst = _dst_dir;
			path_dst+= rev_i->first.substr(_base_dir.size());
			CopyAttributes(path_dst, rev_i->second);
		}

		if (_kind == XK_MOVE) {
			if (S_ISDIR(rev_i->second.mode)) {
				WhatOnErrorWrap<WEK_REMOVE>(_wea_state, _state, _base_host.get(), rev_i->first,
					[&] () mutable 
					{
						_base_host->DirectoryDelete(rev_i->first);
					}
				);
			}
		}
	}
}

void OpXfer::CopyAttributes(const std::string &path_dst, const FileInformation &info)
{
	WhatOnErrorWrap<WEK_SETTIMES>(_wea_state, _state, _dst_host.get(), path_dst,
		[&] () mutable
		{
			_dst_host->SetTimes(path_dst.c_str(), info.access_time, info.modification_time);
		}
	);

	if (( (info.mode | EXTRA_NEEDED_MODE) == info.mode)) {
		return;
	}

	WhatOnErrorWrap<WEK_CHMODE>(_wea_state, _state, _dst_host.get(), path_dst,
		[&] () mutable
		{
			_dst_host->SetMode(path_dst.c_str(), info.mode);
		}
	);

}

void OpXfer::EnsureProgressConsistency()
{
	if (_state.stats.file_complete > _state.stats.file_total) {
		// keep pocker face if file grew while copying
		_state.stats.all_total+= _state.stats.file_complete - _state.stats.file_total;
		_state.stats.file_total = _state.stats.file_complete;
	}
}

bool OpXfer::FileCopyLoop(const std::string &path_src, const std::string &path_dst, FileInformation &info)
{
	unsigned long speed_avg = 0;
	for (IHost *indicted = nullptr;;) try {
		unsigned long long file_complete;
		if (indicted) { // retrying...
			indicted->ReInitialize();
			indicted = _dst_host.get();
			file_complete = _dst_host->GetSize(path_dst);

			ProgressStateUpdate psu(_state);
			_state.stats.file_complete = file_complete;
			EnsureProgressConsistency();
		} else {
			ProgressStateUpdate psu(_state);
			file_complete = _state.stats.file_complete;
		}

		indicted = _base_host.get();
		std::shared_ptr<IFileReader> reader = _base_host->FileGet(path_src, file_complete);
		indicted = _dst_host.get();
		std::shared_ptr<IFileWriter> writer = _dst_host->FilePut(path_dst, info.mode | EXTRA_NEEDED_MODE, file_complete);
		if (!_io_buf.Size())
			throw std::runtime_error("No buffer - no file");

		for (;;) {
			indicted = _base_host.get();
			size_t ask_piece = _io_buf.Size();
			if (info.size < file_complete + ask_piece && info.size > file_complete) {
				// use small buffer if gonna read small piece: IO may have small-read-optimized implementation
				// but ask by one extra byte more to properly detect file being grew while copied
				ask_piece = (info.size - file_complete) + 1;
			}

			std::chrono::milliseconds msec = TimeMSNow();

			const size_t piece = reader->Read(_io_buf.Data(), ask_piece);
			if (piece == 0) {
				if (file_complete < info.size) {
					// protocol returned no read error, but trieved less data then expected, only two reasons possible:
					// - remote file size reduced while copied
					// - protocol implementation misdetected read failure
					// so get actual file size, and if it still bigger than retrieved data size then ring-the-bell
					const auto actual_size = _dst_host->GetSize(path_src);
					if (file_complete < actual_size) {
						info.size = actual_size;
						throw std::runtime_error("Retrieved less data than expected");
					}
					if (file_complete >= actual_size) {
						info.size = file_complete;
					}
				}
				break;
			}

			indicted = _dst_host.get();
			writer->Write(_io_buf.Data(), piece);

			if (piece >= BUFFER_SIZE_GRANULARITY) {
				msec = TimeMSNow() - msec;
				unsigned long prev_bufsize = _io_buf.Size();
				if (msec.count() > 0) {
					unsigned long speed_piece = (1000 * piece) / msec.count();
					if (speed_avg) {
						speed_avg = (3 * speed_avg + speed_piece) / 4;
						unsigned long bufsize_optimal = ((3 * speed_avg) / 4);
						unsigned long bufsize_align = bufsize_optimal % BUFFER_SIZE_GRANULARITY;
						if (bufsize_align) {
							bufsize_optimal-= bufsize_align;
						}
                                                _io_buf.Desire(bufsize_optimal);

					} else {
						speed_avg = speed_piece;
					}

				} else {
					_io_buf.Increase(false);
				}
				if (_io_buf.Size() != prev_bufsize) {
					fprintf(stderr, "NetRocks: IO buffer size changed to %lu\n", _io_buf.Size());
				}
			}

			indicted = nullptr;
			_wea_state.ResetAutoRetryDelay();

			file_complete+= piece;
			ProgressStateUpdate psu(_state);
			_state.stats.file_complete = file_complete;
			_state.stats.all_complete+= piece;
			EnsureProgressConsistency();

			if (piece < ask_piece && file_complete == info.size) {
				// read returned less than was asked, and position is exactly at file size
				// - pretty sure its end of file, so don't iterate to next IO to save time, space and Universe
				// fprintf(stderr, "optimized read completion\n");
				break;
			}
		}
		break;

	} catch (AbortError &) {
		throw;

	} catch (std::exception &ex) {
		if (indicted == nullptr) {
			throw;
		}

		switch (_wea_state.Query(_state, (_direction == XD_UPLOAD) ? WEK_UPLOAD
				: ((_direction == XD_DOWNLOAD) ? WEK_DOWNLOAD : WEK_CROSSLOAD) ,
				ex.what(), path_src, indicted->SiteName())) {

			case WEA_SKIP: {
				ProgressStateUpdate psu(_state);
				_state.stats.count_skips++;
			} return false;

			case WEA_RETRY: {
				ProgressStateUpdate psu(_state);
				_state.stats.count_retries++;
			} break;

			default:
				throw AbortError(); // throw abort to avoid second error warning
		}
	}

	return true;
}

void OpXfer::DirectoryCopy(const std::string &path_dst, const FileInformation &info)
{
	WhatOnErrorWrap<WEK_MAKEDIR>(_wea_state, _state, _dst_host.get(), path_dst,
		[&] () mutable
		{
			_dst_host->DirectoryCreate(path_dst, info.mode | EXTRA_NEEDED_MODE);
		}
	);
}

bool OpXfer::SymlinkCopy(const std::string &path_src, const std::string &path_dst)
{
	std::string symlink_target;
	WhatOnErrorWrap<WEK_SYMLINK_QUERY>(_wea_state, _state, _base_host.get(), path_src,
		[&] () mutable
		{
			symlink_target.clear();
			_base_host->SymlinkQuery(path_src, symlink_target);
		}
	);

	if (symlink_target.empty()) {
		return false;
	}

	std::string orig_symlink_target = symlink_target;

	if (symlink_target[0] == '/') {
		if (_entries.find(symlink_target) ==  _entries.end()) {
			fprintf(stderr, "NetRocks: SymlinkCopy dismiss '%s' [%s]\n",
				path_src.c_str(), orig_symlink_target.c_str());
			return false;
		}
		// absolute target is part of copied directories tree: translate it to relative form
		// /base/dir/copied/sym/link
		// /some/destination/path/copied/sym/link

		symlink_target.erase(0, _base_dir.size());
		for (size_t i = _base_dir.size(); i < path_dst.size(); ++i) {
			if (path_dst[i] == '/') {
				symlink_target.insert(0, "../");
			}
		}
	} else {
		// target is relative: check if it doesnt point outside of copied directories tree
		// ../../../some/relaive/path
		std::string refined = path_src;
		size_t p = refined.rfind('/');
		refined.resize( (p == std::string::npos) ? 0 : p);

		for (size_t i = 0, ii = 0; i != symlink_target.size() + 1; ++i) {
			if (i == symlink_target.size() || symlink_target[i] == '/') {
				if (i > ii) {
					const std::string &component = symlink_target.substr(ii, i - ii);
					if (component == "..") {
						p = refined.rfind('/');
						refined.resize( (p == std::string::npos) ? 0 : p);
					} else if (component != ".") {
						if (!refined.empty() && refined[refined.size() - 1] != '/') {
							refined+= '/';
						}
						refined+= component;
					}
				}
				ii = i + 1;
			}
		}

		if (_entries.find(refined) ==  _entries.end()) {
			fprintf(stderr, "NetRocks: SymlinkCopy dismiss '%s' [%s] refined='%s;\n",
				path_src.c_str(), orig_symlink_target.c_str(), refined.c_str());
			return false;
		}
	}

	fprintf(stderr, "NetRocks: SymlinkCopy '%s' [%s] -> '%s' [%s]\n",
		path_src.c_str(), orig_symlink_target.c_str(), path_dst.c_str(), symlink_target.c_str());

	bool created = false;
	WhatOnErrorWrap<WEK_SYMLINK_CREATE>(_wea_state, _state, _dst_host.get(), path_dst,
		[&] () mutable
		{
			try {
				created = true;
				_dst_host->SymlinkCreate(path_dst, symlink_target);
			} catch (ProtocolUnsupportedError &) {}
		}
	);

	return created;
}

void OpXfer::ForcefullyAbort()
{
	OpBase::ForcefullyAbort();
	_dst_host->Abort();
}
