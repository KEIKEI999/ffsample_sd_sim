﻿/*-----------------------------------------------------------------------*/
/* Low level disk control module for Win32              (C)ChaN, 2019    */
/*-----------------------------------------------------------------------*/

#include "diskio.h"		/* Declarations of disk functions */
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>


#define MAX_DRIVES	10		/* Max number of physical drives to be used */
#define	SZ_BLOCK	128		/* Block size to be returned by GET_BLOCK_SIZE command */

#define SZ_RAMDISK	135		/* Size of drive 0 (RAM disk) [MiB] */
#define SS_RAMDISK	512		/* Initial sector size of drive 0 (RAM disk) [byte] */

#define MIN_READ_ONLY	5	/* Read-only drive from */
#define	MAX_READ_ONLY	5	/* Read-only drive to */



/*--------------------------------------------------------------------------

   Module Private Functions

---------------------------------------------------------------------------*/

#define	BUFSIZE 262144UL	/* Size of data transfer buffer */

typedef struct {
	DSTATUS	status;
	WORD sz_sector;
	LBA_t n_sectors;
	HANDLE h_drive;
} STAT;

static HANDLE hMutex, hTmrThread;
static int Drives;

static volatile STAT Stat[MAX_DRIVES];


static DWORD TmrThreadID;

static BYTE *Buffer, *RamDisk;	/* Poiter to the data transfer buffer and RAM disk */


/*-----------------------------------------------------------------------*/
/* Timer Functions                                                       */
/*-----------------------------------------------------------------------*/


DWORD WINAPI tmr_thread (LPVOID parms)
{
	DWORD dw;
	int drv;

#if 0
	for (;;) {
		Sleep(100);
		for (drv = 1; drv < Drives; drv++) {
			Sleep(1);
			if (Stat[drv].h_drive != INVALID_HANDLE_VALUE && !(Stat[drv].status & STA_NOINIT) && WaitForSingleObject(hMutex, 100) == WAIT_OBJECT_0) {
				if (!DeviceIoControl(Stat[drv].h_drive, IOCTL_STORAGE_CHECK_VERIFY, 0, 0, 0, 0, &dw, 0)) {
					Stat[drv].status |= STA_NOINIT;
				}
				ReleaseMutex(hMutex);
				Sleep(100);
			}
		}
	}
#endif
}



int get_status (
	BYTE pdrv
)
{
	volatile STAT *stat = &Stat[pdrv];
	HANDLE h = stat->h_drive;
	DISK_GEOMETRY_EX parms_ex;
	DISK_GEOMETRY parms;
	DWORD rb;


	stat->status = STA_NOINIT;

	if (pdrv == 0) {	/* RAMDISK */
		if (stat->sz_sector < FF_MIN_SS || stat->sz_sector > FF_MAX_SS) return 0;
		stat->n_sectors = (LBA_t)((QWORD)SZ_RAMDISK * 0x100000 / SS_RAMDISK);
		return 1;
	}

	if (pdrv > 3) {
		stat->n_sectors = 1024 * 1024;
		stat->sz_sector = 512;
		stat->status = 0;
	}

	/* Get drive size */
	if (!DeviceIoControl(h, IOCTL_STORAGE_CHECK_VERIFY, 0, 0, 0, 0, &rb, 0)) return 0;
	if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, 0, 0, &parms_ex, sizeof parms_ex, &rb, 0)) {
		stat->sz_sector = (WORD)parms_ex.Geometry.BytesPerSector;
		stat->n_sectors = (LBA_t)(parms_ex.DiskSize.QuadPart / stat->sz_sector);
	} else {
		if (!DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY, 0, 0, &parms, sizeof parms, &rb, 0)) return 0;
		stat->n_sectors = parms.SectorsPerTrack * parms.TracksPerCylinder * (LBA_t)parms.Cylinders.QuadPart;
		stat->sz_sector = (WORD)parms.BytesPerSector;
	}
	if (stat->sz_sector < FF_MIN_SS || stat->sz_sector > FF_MAX_SS) return 0;

	/* Get write protect status */
	stat->status = DeviceIoControl(h, IOCTL_DISK_IS_WRITABLE, 0, 0, 0, 0, &rb, 0) ? 0 : STA_PROTECT;
	if (pdrv >= MIN_READ_ONLY && pdrv <= MAX_READ_ONLY) stat->status = STA_PROTECT;
	return 1;
}




/*--------------------------------------------------------------------------

   Public Functions

---------------------------------------------------------------------------*/
static HANDLE hSD[10];

void dismount_volume(const TCHAR drvlet[])
{
	int i;
	DWORD dummy;
	for (i = 0; i < 10; i++) {
		HANDLE *pSD = &hSD[i];
		WCHAR szlogicalÐrvName[10] = L"\\\\.\\d:";
		if (drvlet[i] == '\0') {
			break;
		}
		szlogicalÐrvName[4] = drvlet[i];
		if ((*pSD != 0)) {
			DeviceIoControl(*pSD, FSCTL_UNLOCK_VOLUME, 0, 0, 0, 0, &dummy, 0);
			CloseHandle(*pSD);
			*pSD = 0;
		}
		*pSD = CreateFileW(szlogicalÐrvName, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
		DeviceIoControl(*pSD, FSCTL_LOCK_VOLUME, 0, 0, 0, 0, &dummy, 0);
		DeviceIoControl(*pSD, FSCTL_DISMOUNT_VOLUME, 0, 0, 0, 0, &dummy, 0);
	}
}


/*-----------------------------------------------------------------------*/
/* Initialize Windows disk accesss layer                                 */
/*-----------------------------------------------------------------------*/

int assign_drives (void)
{
	BYTE pdrv, ndrv;
	WCHAR str[50];
	HANDLE h;
	OSVERSIONINFO vinfo = { sizeof (OSVERSIONINFO) };


	hMutex = CreateMutex(0, 0, 0);
	if (hMutex == INVALID_HANDLE_VALUE) return 0;

	Buffer = VirtualAlloc(0, BUFSIZE, MEM_COMMIT, PAGE_READWRITE);
	if (!Buffer) return 0;

	RamDisk = VirtualAlloc(0, SZ_RAMDISK * 0x100000, MEM_COMMIT, PAGE_READWRITE);
	if (!RamDisk) return 0;
	Stat[0].sz_sector = SS_RAMDISK;

	//if (GetVersionEx(&vinfo) == FALSE) return 0;
	//ndrv = vinfo.dwMajorVersion > 5 ? 1 : MAX_DRIVES;
	ndrv = MAX_DRIVES;

	for (pdrv = 0; pdrv < ndrv; pdrv++) {
		if (pdrv == 0) {	/* \\.\PhysicalDrive0 is never mapped to disk function, but RAM disk is mapped to pd#0 instead. */
			swprintf(str, 50, L"RAM Disk");
		} else if(pdrv <= 3){			/* \\.\PhysicalDrive<n> (n=1..) are mapped to disk funtion pd#<n>. */
			swprintf(str, 50, L"\\\\.\\PhysicalDrive%u", pdrv);
			if (Stat[pdrv].h_drive != 0) {
				CloseHandle(Stat[pdrv].h_drive);
			}
			h = CreateFileW(str, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, 0);
			if (h != INVALID_HANDLE_VALUE)
				Stat[pdrv].h_drive = h;
		}else {
			swprintf(str, 50, L"PhysicalDrive%u", pdrv);
			if (Stat[pdrv].h_drive != 0) {
				CloseHandle(Stat[pdrv].h_drive);
			}
			h = CreateFileW(str, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, 0);
			if (h != INVALID_HANDLE_VALUE)
				Stat[pdrv].h_drive = h;
		}
		wprintf(L"PD#%u <== %s", pdrv, str);
		if (get_status(pdrv)) {
			wprintf(L" (%uMB, %u bytes * %I64u sectors)\n", (UINT)((LONGLONG)Stat[pdrv].sz_sector * Stat[pdrv].n_sectors / 1024 / 1024), Stat[pdrv].sz_sector, (QWORD)Stat[pdrv].n_sectors);
		} else {
			wprintf(L" (Not Ready)\n");
		}
	}

	hTmrThread = CreateThread(0, 0, tmr_thread, 0, 0, &TmrThreadID);
	if (hTmrThread == INVALID_HANDLE_VALUE) pdrv = 0;

	if (ndrv > 1) {
		if (pdrv == 1) {
			wprintf(L"\nYou must run the program as Administrator to access the physical drives.\n");
		}
	} else {
		wprintf(L"\nOn the Windows Vista and later, you cannot access the physical drives.\nUse Windows NT/2k/XP instead.\n");
	}

	Drives = pdrv;
	return pdrv;
}





/*-----------------------------------------------------------------------*/
/* Initialize Disk Drive                                                 */
/*-----------------------------------------------------------------------*/

DSTATUS disk_initialize (
	BYTE pdrv		/* Physical drive nmuber */
)
{
	DSTATUS sta;


	if (WaitForSingleObject(hMutex, 5000) != WAIT_OBJECT_0) {
		return STA_NOINIT;
	}

	if (pdrv >= Drives) {
		sta = STA_NOINIT;
	} else {
		get_status(pdrv);
		if (pdrv == 0) Stat[pdrv].status = 0;
		sta = Stat[pdrv].status;
	}

	ReleaseMutex(hMutex);
	return sta;
}



/*-----------------------------------------------------------------------*/
/* Get Disk Status                                                       */
/*-----------------------------------------------------------------------*/

DSTATUS disk_status (
	BYTE pdrv		/* Physical drive nmuber (0) */
)
{
	DSTATUS sta;


	if (pdrv >= Drives) {
		sta = STA_NOINIT;
	} else {
		sta = Stat[pdrv].status;
	}

	return sta;
}



/*-----------------------------------------------------------------------*/
/* Read Sector(s)                                                        */
/*-----------------------------------------------------------------------*/

DRESULT disk_read (
	BYTE pdrv,			/* Physical drive nmuber (0) */
	BYTE *buff,			/* Pointer to the data buffer to store read data */
	LBA_t sector,		/* Start sector number (LBA) */
	UINT count			/* Number of sectors to read */
)
{
	DWORD nc, rnc;
	LARGE_INTEGER ofs;
	DSTATUS res;


	if (pdrv >= Drives || Stat[pdrv].status & STA_NOINIT || WaitForSingleObject(hMutex, 3000) != WAIT_OBJECT_0) {
		return RES_NOTRDY;
	}

	nc = (DWORD)count * Stat[pdrv].sz_sector;
	ofs.QuadPart = (QWORD)sector * Stat[pdrv].sz_sector;
	if (pdrv) {	/* Physical dirve */
		if (nc > BUFSIZE) {
			res = RES_PARERR;
		} else {
			if (SetFilePointer(Stat[pdrv].h_drive, ofs.LowPart, &ofs.HighPart, FILE_BEGIN) != ofs.LowPart) {
				res = RES_ERROR;
			} else {
				if (!ReadFile(Stat[pdrv].h_drive, Buffer, nc, &rnc, 0) || nc != rnc) {
					res = RES_ERROR;
				} else {
					memcpy(buff, Buffer, nc);
					res = RES_OK;
				}
			}
		}
	} else {	/* RAM disk */
		if (ofs.QuadPart >= (QWORD)SZ_RAMDISK * 1024 * 1024) {
			res = RES_ERROR;
		} else {
			memcpy(buff, RamDisk + ofs.LowPart, nc);
			res = RES_OK;
		}
	}

	ReleaseMutex(hMutex);
	return res;
}



/*-----------------------------------------------------------------------*/
/* Write Sector(s)                                                       */
/*-----------------------------------------------------------------------*/

DRESULT disk_write (
	BYTE pdrv,			/* Physical drive nmuber (0) */
	const BYTE *buff,	/* Pointer to the data to be written */
	LBA_t sector,		/* Start sector number (LBA) */
	UINT count			/* Number of sectors to write */
)
{
	DWORD nc = 0, rnc;
	LARGE_INTEGER ofs;
	DRESULT res;


	if (pdrv >= Drives || Stat[pdrv].status & STA_NOINIT || WaitForSingleObject(hMutex, 3000) != WAIT_OBJECT_0) {
		return RES_NOTRDY;
	}

	res = RES_OK;
	if (Stat[pdrv].status & STA_PROTECT) {
		res = RES_WRPRT;
	} else {
		nc = (DWORD)count * Stat[pdrv].sz_sector;
		if (nc > BUFSIZE) res = RES_PARERR;
	}

	ofs.QuadPart = (QWORD)sector * Stat[pdrv].sz_sector;
	if (pdrv >= 1) {	/* Physical drives */
		if (pdrv >= MIN_READ_ONLY && pdrv <= MAX_READ_ONLY) res = RES_WRPRT;
		if (res == RES_OK) {
			if (SetFilePointer(Stat[pdrv].h_drive, ofs.LowPart, &ofs.HighPart, FILE_BEGIN) != ofs.LowPart) {
				res = RES_ERROR;
			} else {
				memcpy(Buffer, buff, nc);
				{
					int i;
					//DWORD dummy;
					//DeviceIoControl( Stat[pdrv].h_drive, FSCTL_LOCK_VOLUME, 0, 0, 0, 0, &dummy, 0 );
					//DeviceIoControl( Stat[pdrv].h_drive, FSCTL_DISMOUNT_VOLUME, 0, 0, 0, 0, &dummy, 0 );
					for (i = 0; i < 10; i++) {
						if (!WriteFile(Stat[pdrv].h_drive, Buffer, nc, &rnc, 0) || nc != rnc) {
							LPVOID lpMsgBuf;
							res = RES_ERROR;

							FormatMessage(
								FORMAT_MESSAGE_ALLOCATE_BUFFER  //      テキストのメモリ割り当てを要求する
								| FORMAT_MESSAGE_FROM_SYSTEM    //      エラーメッセージはWindowsが用意しているものを使用
								| FORMAT_MESSAGE_IGNORE_INSERTS,//      次の引数を無視してエラーコードに対するエラーメッセージを作成する
								NULL, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),//   言語を指定
								(LPTSTR)&lpMsgBuf,                          //      メッセージテキストが保存されるバッファへのポインタ
								0,
								NULL);
							printf((char*)lpMsgBuf);

						}
						else {
							res = RES_OK;
							break;
						}
					}
					//DeviceIoControl( Stat[pdrv].h_drive, FSCTL_UNLOCK_VOLUME, 0, 0, 0, 0, &dummy, 0 );
				}
				FlushFileBuffers(Stat[pdrv].h_drive);
			}
		}
	} else {	/* RAM disk */
		if (ofs.QuadPart >= (QWORD)SZ_RAMDISK * 1024 * 1024) {
			res = RES_ERROR;
		} else {
			memcpy(RamDisk + ofs.LowPart, buff, nc);
			res = RES_OK;
		}
	}

	ReleaseMutex(hMutex);
	return res;
}



/*-----------------------------------------------------------------------*/
/* Miscellaneous Functions                                               */
/*-----------------------------------------------------------------------*/

DRESULT disk_ioctl (
	BYTE pdrv,		/* Physical drive nmuber (0) */
	BYTE ctrl,		/* Control code */
	void *buff		/* Buffer to send/receive data */
)
{
	DRESULT res = RES_PARERR;


	if (pdrv == 0 && ctrl == 200) {
		HANDLE h;
		DWORD br;
		WORD ramss;

		if (pdrv == 0) {
			h = CreateFileW(buff, GENERIC_READ, 0, 0, OPEN_EXISTING, 0, 0);
			if (h != INVALID_HANDLE_VALUE) {
				if (ReadFile(h, RamDisk, SZ_RAMDISK * 1024 * 1024, &br, 0)) {
					Stat[pdrv].status = STA_NOINIT;
					if (br < SZ_RAMDISK * 1024 * 1024) {
						ramss = RamDisk[0x0B] + 256 * RamDisk[0x0C];
						Stat[0].sz_sector = (ramss == 1024 || ramss == 2048 || ramss == 4096) ? ramss : 512;
						res = RES_OK;
					}
				}
				CloseHandle(h);
			}
		}
		return res;
	}

	if (pdrv >= Drives || (Stat[pdrv].status & STA_NOINIT)) {
		return RES_NOTRDY;
	}

	switch (ctrl) {
	case CTRL_SYNC:			/* Nothing to do */
		res = RES_OK;
		break;

	case GET_SECTOR_COUNT:	/* Get number of sectors on the drive */
		*(LBA_t*)buff = Stat[pdrv].n_sectors;
		res = RES_OK;
		break;

	case GET_SECTOR_SIZE:	/* Get size of sector for generic read/write */
		*(WORD*)buff = Stat[pdrv].sz_sector;
		res = RES_OK;
		break;

	case GET_BLOCK_SIZE:	/* Get internal block size in unit of sector */
		*(DWORD*)buff = SZ_BLOCK;
		res = RES_OK;
		break;
	}

	return res;
}



