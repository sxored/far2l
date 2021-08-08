#pragma once

/*
copy.hpp

class ShellCopy - Копирование файлов
*/
/*
Copyright (c) 1996 Eugene Roshal
Copyright (c) 2000 Far Group
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
3. The name of the authors may not be used to endorse or promote products
   derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "dizlist.hpp"
#include "udlist.hpp"
#include "flink.hpp"
class Panel;

enum COPY_CODES
{
	COPY_CANCEL,
	COPY_NEXT,
	COPY_NOFILTER,                              // не считать размеры, т.к. файл не прошел по фильтру
	COPY_FAILURE,
	COPY_FAILUREREAD,
	COPY_SUCCESS,
	COPY_SUCCESS_MOVE,
	COPY_RETRY,
};

enum COPY_FLAGS
{
	FCOPY_COPYTONUL               	= 0x00000001, // Признак копирования в NUL
	FCOPY_CURRENTONLY             	= 0x00000002, // Только текщий?
	FCOPY_ONLYNEWERFILES          	= 0x00000004, // Copy only newer files
	FCOPY_OVERWRITENEXT           	= 0x00000008, // Overwrite all
	FCOPY_LINK                    	= 0x00000010, // создание линков
	FCOPY_MOVE                    	= 0x00000040, // перенос/переименование
	FCOPY_DIZREAD                 	= 0x00000080, //
	FCOPY_COPYACCESSMODE          	= 0x00000100, // [x] Copy files access mode
	FCOPY_NOSHOWMSGLINK           	= 0x00000200, // не показывать месаги при ликовании
	FCOPY_VOLMOUNT                	= 0x00000400, // операция монтированния тома
	FCOPY_STREAMSKIP              	= 0x00000800, // потоки
	FCOPY_STREAMALL               	= 0x00001000, // потоки
	FCOPY_SKIPSETATTRFLD          	= 0x00002000, // больше не пытаться ставить атрибуты для каталогов - когда нажали Skip All
	FCOPY_COPYSYMLINKCONTENTS     	= 0x00004000, // Copy symbolics links content instead of making new links
	FCOPY_COPYSYMLINKCONTENTSOUTER	= 0x00008000, // Copy remote (to this copy operation) symbolics links content, make relative links for local ones
	FCOPY_WRITETHROUGH            	= 0x00040000, // disable write cache
	FCOPY_COPYXATTR               	= 0x00080000, // copy extended attributes
	FCOPY_SPARSEFILES             	= 0x00100000, // allow producing sparse files
	FCOPY_USECOW                  	= 0x00200000, // enable COW funcionality if FS supports it
	FCOPY_COPYLASTTIME            	= 0x10000000, // При копировании в несколько каталогов устанавливается для последнего.
	FCOPY_UPDATEPPANEL            	= 0x80000000, // необходимо обновить пассивную панель
};

class ShellCopyFileExtendedAttributes
{
	FileExtendedAttributes _xattr;
	bool _apply;

	public:
	ShellCopyFileExtendedAttributes(File &f);
	void ApplyToCopied(File &f);
};

struct ShellCopyBuffer
{
	ShellCopyBuffer();
	~ShellCopyBuffer();

	const DWORD Capacity;

	DWORD Size;

private:
	char * const Buffer;

public:
	char *const Ptr;
};

class ShellFileTransfer
{
	const wchar_t *_SrcName;
	const FARString &_strDestName;
	ShellCopyBuffer &_CopyBuffer;
	DWORD _Flags;
	const FAR_FIND_DATA_EX &_SrcData;

	clock_t _Stopwatch = 0;
	int64_t _AppendPos = -1;
	DWORD _DstFlags = 0;
	DWORD _ModeToCreateWith;

	File _SrcFile, _DestFile;
	bool _LastWriteWasHole = false;
	bool _Done = false;
	std::unique_ptr<ShellCopyFileExtendedAttributes> _XAttrCopyPtr;

	void Undo();
	void RetryCancel(const wchar_t *Text, const wchar_t *Object);
	DWORD PieceWrite(const void *Data, DWORD Size);
	DWORD PieceWriteHole(DWORD Size);
	DWORD PieceCopy();
	void ProgressUpdate(bool force);

public:
	ShellFileTransfer(const wchar_t *SrcName, const FAR_FIND_DATA_EX &SrcData,
		const FARString &strDestName, bool Append, ShellCopyBuffer &CopyBuffer, DWORD Flags);
	~ShellFileTransfer();

	void Do();
};

class ShellCopy
{
		DWORD Flags;
		Panel *SrcPanel,*DestPanel;
		int SrcPanelMode,DestPanelMode;
		int SrcDriveType,DestDriveType;
		FARString strDestFSName;
		char   *sddata; // Security
		DizList DestDiz;
		FARString strDestDizPath;
		FARString strCopiedName;
		FARString strRenamedName;
		FARString strRenamedFilesPath;
		int OvrMode;
		int ReadOnlyOvrMode;
		int ReadOnlyDelMode;
		int SkipMode;          // ...для пропуска при копировании залоченных файлов.
		int SkipEncMode;
		int SkipDeleteMode;
		int SelectedFolderNameLength;
		UserDefinedList DestList;
		// тип создаваемого репарспоинта.
		// при AltF6 будет то, что выбрал юзер в диалоге,
		// в остальных случаях - RP_EXACTCOPY - как у источника
		ReparsePointTypes RPT;
		ShellCopyBuffer CopyBuffer;

		COPY_CODES CopyFileTree(const wchar_t *Dest);
		COPY_CODES ShellCopyOneFile(const wchar_t *Src,
		                            const FAR_FIND_DATA_EX &SrcData,
		                            FARString &strDest,
		                            int KeepPathPos, int Rename);
		COPY_CODES ShellCopyOneFileWithRoot(const wchar_t *Root, const wchar_t *Src,
		                            const FAR_FIND_DATA_EX &SrcData,
		                            FARString &strDest,
		                            int KeepPathPos, int Rename);
		COPY_CODES ShellCopyOneFileWithRootNoRetry(const wchar_t *Root, const wchar_t *Src,
		                            const FAR_FIND_DATA_EX &SrcData,
		                            FARString &strDest,
		                            int KeepPathPos, int Rename);

		COPY_CODES CheckStreams(const wchar_t *Src,const wchar_t *DestPath);

		int  ShellCopyFile(const wchar_t *SrcName,const FAR_FIND_DATA_EX &SrcData,
		                   FARString &strDestName,int Append);

		int  DeleteAfterMove(const wchar_t *Name,DWORD Attr);
		void SetDestDizPath(const wchar_t *DestPath);
		int  AskOverwrite(const FAR_FIND_DATA_EX &SrcData,const wchar_t *SrcName,const wchar_t *DestName,
		                  DWORD DestAttr,int SameName,int Rename,int AskAppend,
		                  int &Append,FARString &strNewName,int &RetCode);
		bool CalcTotalSize();
		bool ShellSetAttr(const wchar_t *Dest,DWORD Attr);
		void CheckUpdatePanel(); // выставляет флаг FCOPY_UPDATEPPANEL
		
		COPY_CODES CreateSymLink(const char *ExistingName, const wchar_t *NewName, const FAR_FIND_DATA_EX &SrcData);
		COPY_CODES CopySymLink(const wchar_t *Root, const wchar_t *ExistingName, 
					const wchar_t *NewName, ReparsePointTypes LinkType, const FAR_FIND_DATA_EX &SrcData);
	public:
		ShellCopy(Panel *SrcPanel,int Move,int Link,int CurrentOnly,int Ask,
		          int &ToPlugin, const wchar_t *PluginDestPath, bool ToSubdir=false);
		~ShellCopy();
};

LONG_PTR WINAPI CopyDlgProc(HANDLE hDlg,int Msg,int Param1,LONG_PTR Param2);
