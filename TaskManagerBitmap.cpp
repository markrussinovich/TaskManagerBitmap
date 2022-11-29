//--------------------------------------------------------------------------------
// 
// TaskManagerBitmap
//
// By Mark Russinovich 
// December 2020
//
// Takes a bitmap that is the size or width of Task Managers's CPU core view (ideally with single cells 
// showing instantaneous percent CPU) and scrolls the bitmap using threads that consume CPU 
// based on the pixel's greyscale (black is 100% CPU)
//
//--------------------------------------------------------------------------------
#include <Windows.h>
#include <processthreadsapi.h>
#include <iostream>

// Shades of grey
#define GREYSCALE       8
volatile DWORD* CpuPixels;


typedef struct {
	DWORD       ProcessId;
	HWND        hWnd;
} HWND_CONTEXT, * PHWND_CONTEXT;


//--------------------------------------------------------------------------------
//
// IsMainWindow
//
// Is window parentless and visible. 
//
//--------------------------------------------------------------------------------
BOOL IsMainWindow(
	HWND handle
)
{
	return GetWindow(handle, GW_OWNER) == (HWND)0 && IsWindowVisible(handle);
}


//--------------------------------------------------------------------------------
//
// EnumWindowsCallback
//
// Scan windows of process looking for main one. 
//
//--------------------------------------------------------------------------------
BOOL CALLBACK EnumWindowsCallback(
	HWND handle,
	LPARAM lParam
)
{
	PHWND_CONTEXT   context = (PHWND_CONTEXT)lParam;
	DWORD          processId = 0;

	GetWindowThreadProcessId(handle, &processId);
	if (context->ProcessId != processId || !IsMainWindow(handle))
		return TRUE;
	context->hWnd = handle;
	return FALSE;
}


//--------------------------------------------------------------------------------
//
// FindMainWindow
//
// Enumerate windows. 
//
//--------------------------------------------------------------------------------
HWND FindMainWindow(
	DWORD ProcessId
)
{
	HWND_CONTEXT context;

	context.ProcessId = ProcessId;
	context.hWnd = 0;
	EnumWindows(EnumWindowsCallback, (LPARAM)&context);
	return context.hWnd;
}

//--------------------------------------------------------------------------------
//
// PixelCpuThread
//
// Thread that runs affinitized to a specific core. It monitors
// its cell in a CpuPixel array to see how much CPU it should burn
//
//--------------------------------------------------------------------------------
DWORD WINAPI PixelCpuThread(
	_In_ LPVOID lpParameter
)
{
	DWORD       cpuNumber = (DWORD)(DWORD_PTR)lpParameter;
	ULONGLONG   startTick;

	while (1) {

		startTick = GetTickCount64();
		while (GetTickCount64() - startTick < CpuPixels[cpuNumber] * (100 / GREYSCALE));

		Sleep(100 - CpuPixels[cpuNumber] * (100 / GREYSCALE));
	}
	return 0;
}



//--------------------------------------------------------------------------------
//
// LaunchBitmapThreads
//
// Launch a thread pinned to each CPU on the system, following the ordering
// used by Task Manager's CPU view. 
//
//--------------------------------------------------------------------------------
DWORD
LaunchBitmapThreads(
	volatile DWORD** CpuPixels
)
{
	PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX curProcInfo;
	PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX logGroupProcInfo;
	DWORD           offset = 0;
	GROUP_AFFINITY  groupAffinity;
	LPPROC_THREAD_ATTRIBUTE_LIST    attrList;
	SIZE_T          attrListSize = 0;
	HANDLE          hThread;
	DWORD           returnLength = 0;
	ULONG_PTR       cpu, cpuNumber;
	PPROCESSOR_RELATIONSHIP      groupRelationship;

	//

	GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &returnLength);
	logGroupProcInfo = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)malloc(returnLength);
	GetLogicalProcessorInformationEx(RelationProcessorCore, logGroupProcInfo, &returnLength);
	offset = 0;
	int maxCpus = 0;
	int maxGroups = -1;
	static int groupsAll[100];
	int groupCurrent = -1;
	while (offset < returnLength) {
		curProcInfo = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)((DWORD_PTR)logGroupProcInfo + offset);
		groupRelationship = &curProcInfo->Processor;
		groupCurrent = (int)groupRelationship->GroupMask->Group;
		if (maxGroups < groupCurrent) {
			maxGroups = groupCurrent;
			groupsAll[groupCurrent] = 0;
		}
		printf("Core is hyper-threaded: %d\n", groupRelationship->Flags);
		offset += curProcInfo->Size;
		groupsAll[groupCurrent] = groupsAll[groupCurrent] + groupRelationship->Flags + 1;
		maxCpus = maxCpus + groupRelationship->Flags + 1;
	}

	printf("Cores found: %d. Processor Groups found: %d.\n", maxCpus, maxGroups + 1);
	for (int i = 0; i <= maxGroups; i++) {
		printf("Processor Group %d has %d cores.\n", i, groupsAll[i]);
	}

	*CpuPixels = 0;
	curProcInfo = logGroupProcInfo;
	cpuNumber = 0;

	for (int i = 0; i <= maxGroups; i++) {
		for (cpu = 0; cpu < groupsAll[i]; cpu++) {
			*CpuPixels = (DWORD*)realloc((PVOID)*CpuPixels, (cpuNumber + 1) * sizeof(DWORD));
			memset((PVOID)*CpuPixels, 0, (cpuNumber + 1) * sizeof(DWORD));

			InitializeProcThreadAttributeList(NULL, 1, 0, &attrListSize);
			attrList = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attrListSize);
			InitializeProcThreadAttributeList(attrList, 1, 0, &attrListSize);
			memset(&groupAffinity, 0, sizeof(groupAffinity));
			groupAffinity.Group = i;
			groupAffinity.Mask = (KAFFINITY)1 << cpu;
			UpdateProcThreadAttribute(attrList, 0, PROC_THREAD_ATTRIBUTE_GROUP_AFFINITY,
				&groupAffinity, sizeof(groupAffinity), NULL, NULL);

			hThread = CreateRemoteThreadEx(GetCurrentProcess(), 0, 0, PixelCpuThread, (PVOID)(DWORD_PTR)cpuNumber,
				0, attrList, NULL);
			if (hThread)
			{
				CloseHandle(hThread);
				hThread = NULL;
			}
			DeleteProcThreadAttributeList(attrList);
			free(attrList);
			cpuNumber++;
		}
	}

	offset += curProcInfo->Size;
	curProcInfo = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)((DWORD_PTR)curProcInfo + curProcInfo->Size);
	return (DWORD)maxCpus;
}


//--------------------------------------------------------------------------------
//
// Main
//
// Read bitmap, spawn a thread per core pinned to the core, and then
// update the CPU activity map to display the bitmap on Task Manager
//
//--------------------------------------------------------------------------------
int main(int argc, char* argv[])
{
	DWORD           offset;
	DWORD           maxCpus;
	int             width, x, y, processId, averageColor;
	HDC             hScreenDC, hDstDC, hOrigDC;
	HWND            hWnd = NULL;
	BITMAP          bitmap;
	HBITMAP         hBitmap = NULL, hNewBitmap;
	RECT            rcClient, rcBitmap;
	BOOL            scrollHorizontal;
	COLORREF        pixel, greyPixel;
	float           scaleFactor;

	//
	// Width is the width of Task Manager's CPU activity array 
	// 
	if (argc < 3) {

		printf("Usage: %s <bitmap> <width>", argv[0]);
		return -1;
	}
	processId = atoi(argv[1]);
	width = atoi(argv[2]);


	//
	// Spawn a thread pinned to each CPU, identifying them by their CPU number index
	// into the CPU array. Task manager shows CPUs ordered by their NUMA node. 
	//
	maxCpus = LaunchBitmapThreads(&CpuPixels);

	//
	// Load the bitmap
	//
	hScreenDC = GetDC(NULL);
	hOrigDC = CreateCompatibleDC(hScreenDC);
	hDstDC = CreateCompatibleDC(hOrigDC);

	if (processId != 0) {

		hWnd = FindMainWindow(processId);
		if (hWnd == NULL) {

			printf("Unable to find windows for process %d\n", processId);
		}
		hOrigDC = GetDC(hWnd);
		GetClientRect(hWnd, &rcClient);
		if (width / rcClient.right)
			scaleFactor = (float)width / rcClient.right;
		else
			scaleFactor = (float)(maxCpus / width) / rcClient.bottom;
		rcBitmap.right = width;
		rcBitmap.bottom = maxCpus / width;
		hNewBitmap = CreateCompatibleBitmap(hOrigDC, rcBitmap.right, rcBitmap.bottom);
		SelectObject(hDstDC, hNewBitmap);
		StretchBlt(hDstDC, 0, 0, rcBitmap.right, rcBitmap.bottom, hOrigDC, 0, 0, rcClient.right, rcClient.bottom, SRCCOPY);
	}
	else {
		hBitmap = (HBITMAP)LoadImageA(NULL, argv[1], IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE);
		if (hBitmap == NULL) {

			printf("Error loading %s: %d\n", argv[1], GetLastError());
			return -1;
		}
		GetObject(hBitmap, sizeof(BITMAP), &bitmap);
		SelectObject(hOrigDC, hBitmap);

		if (bitmap.bmWidth > bitmap.bmHeight) {

			scrollHorizontal = TRUE;
			scaleFactor = (float)(maxCpus / width) / bitmap.bmHeight;
		}
		else {

			scaleFactor = (float)width / bitmap.bmWidth;
		}
		rcBitmap.right = (int)((float)bitmap.bmWidth * scaleFactor);
		rcBitmap.bottom = (int)(((float)bitmap.bmHeight) * scaleFactor);
		hNewBitmap = CreateCompatibleBitmap(hOrigDC, rcBitmap.right, rcBitmap.bottom);
		SelectObject(hDstDC, hNewBitmap);
		SetStretchBltMode(hDstDC, COLORONCOLOR);
		StretchBlt(hDstDC, 0, 0, rcBitmap.right, rcBitmap.bottom, hOrigDC, 0, 0,
			bitmap.bmWidth, bitmap.bmHeight, SRCCOPY | CAPTUREBLT);
		scrollHorizontal = bitmap.bmWidth > bitmap.bmHeight;
	}

	//
	// Loop the bitmap through the CPU activity array either horizontally or vertically
	// depending on dimensions of bitmap
	//
	offset = 0;
	while (TRUE) {
		for (y = 0; y < (int)maxCpus / width; y++) {
#if _DEBUG
			printf("\n[%d] ", y);
#endif
			for (x = 0; x < width; x++) {
				if (hBitmap) {
					if (scrollHorizontal)
						pixel = GetPixel(hDstDC, (x + offset) % rcBitmap.right, y);
					else
						pixel = GetPixel(hDstDC, x, (y - offset) % rcBitmap.bottom);
				}
				else {

					pixel = GetPixel(hDstDC, x, y);
				}
				averageColor = (GetRValue(pixel) + GetGValue(pixel) + GetBValue(pixel)) / 3;
				greyPixel = RGB(averageColor, averageColor, averageColor);
				CpuPixels[y * width + x] = GREYSCALE - greyPixel / (0xffffff / GREYSCALE);
#if _DEBUG
				printf("%d ", CpuPixels[y * width + x]);
#endif
			}
		}
		Sleep(500);
		if (processId) {

			StretchBlt(hDstDC, 0, 0, width, maxCpus / width, hOrigDC, 0, 0, rcClient.right, rcClient.bottom, SRCCOPY);
		}
		else {
			offset++;
		}
	}
}
