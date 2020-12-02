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

volatile DWORD* CpuPixels;

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
    DWORD   cpuNumber = (DWORD)(DWORD_PTR)lpParameter;
    DWORD   startTick;

    while( 1 ) {

        startTick = GetTickCount();
        while( GetTickCount() - startTick < CpuPixels[cpuNumber] * 25 );

        Sleep( 100 - CpuPixels[cpuNumber] * 25 );
    }
    return 0;
}


//--------------------------------------------------------------------------------
//
// Main
//
// Read bitmap, spawn a thread per core pinned to the core, and then
// update the CPU activity map to display the bitmap on Task Manager
//
//--------------------------------------------------------------------------------
int main( int argc, char* argv[] )
{
    ULONG_PTR       cpu;
    LPPROC_THREAD_ATTRIBUTE_LIST    attrList;
    SIZE_T          attrListSize;
    DWORD           offset, returnLength = 0;
    GROUP_AFFINITY  groupAffinity;
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX logProcInfo, curProcInfo;
    PNUMA_NODE_RELATIONSHIP  numaRelationship;
    DWORD           cpuNumber = 0, maxCpus = 0;
    int             width, x, y;
    HDC             hScreenDC, hSrcDC;
    BITMAP          bitmap;
    HBITMAP         hBitmap;
    COLORREF        pixel, greyPixel;

    //
    // Width is the width of Task Manager's CPU activity array 
    // 
    if( argc < 3 ) {

        printf( "Usage: %s <bitmap> <width>", argv[0] );
        return -1;
    }

    width = atoi( argv[2] );

    //
    // First, get active CPU count
    //
    GetLogicalProcessorInformationEx( RelationNumaNode, NULL, &returnLength );
    logProcInfo = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)malloc( returnLength );
    GetLogicalProcessorInformationEx( RelationNumaNode, logProcInfo, &returnLength );
    offset = 0;
    while( offset < returnLength ) {

        curProcInfo = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)((DWORD_PTR)logProcInfo + offset);
        numaRelationship = &curProcInfo->NumaNode;
        //printf("%d: %llx\n", numaRelationship->NodeNumber, (DWORD64) numaRelationship->GroupMask.Mask);
        for( cpu = 0; cpu < sizeof( numaRelationship->GroupMask.Mask ) * 8; cpu++ ) {

            if( (1ULL << cpu) & numaRelationship->GroupMask.Mask ) {

                maxCpus++;
            }
        }
        offset += curProcInfo->Size;
    }

    //
    // Allocate CPU activity array. We'll use a scale of 0-4 (4 being 100% or black)
    // for the cell values
    //
    CpuPixels = (DWORD*)malloc( maxCpus * sizeof( DWORD ) );
    memset( (PVOID)CpuPixels, 0, maxCpus * sizeof( DWORD ) );

    //
    // Load the bitmap
    //
    hBitmap = (HBITMAP)LoadImageA( NULL, argv[1], IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE );
    if( hBitmap == NULL ) {

        printf( "Error loading %s: %d\n", argv[1], GetLastError() );
        return -1;
    }
    GetObject( hBitmap, sizeof( BITMAP ), &bitmap );
    hScreenDC = GetDC( NULL );
    hSrcDC = CreateCompatibleDC( hScreenDC );
    SelectObject( hSrcDC, hBitmap );

    //
    // Spawn a thread pinned to each CPU, identifying them by their CPU number index
    // into the CPU array. Task manager shows CPUs ordered by their NUMA node. 
    //
    curProcInfo = logProcInfo;
    offset = 0;
    while( offset < returnLength ) {

        numaRelationship = &curProcInfo->NumaNode;
        for( cpu = 0; cpu < sizeof( numaRelationship->GroupMask.Mask ) * 8; cpu++ ) {

            if( (1ULL << cpu) & numaRelationship->GroupMask.Mask ) {

                InitializeProcThreadAttributeList( NULL, 1, 0, &attrListSize );
                attrList = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc( attrListSize );
                InitializeProcThreadAttributeList( attrList, 1, 0, &attrListSize );
                memset( &groupAffinity, 0, sizeof( groupAffinity ) );
                groupAffinity.Group = numaRelationship->GroupMask.Group;
                groupAffinity.Mask = (KAFFINITY)1 << cpu;
                UpdateProcThreadAttribute( attrList, 0, PROC_THREAD_ATTRIBUTE_GROUP_AFFINITY,
                    &groupAffinity, sizeof( groupAffinity ), NULL, NULL );

                CreateRemoteThreadEx( GetCurrentProcess(), 0, 0, PixelCpuThread, (PVOID)(DWORD_PTR) cpuNumber,
                    0, attrList, NULL );
                DeleteProcThreadAttributeList( attrList );
                free( attrList );
                cpuNumber++;
            }
        }
        offset += curProcInfo->Size;
        curProcInfo = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)((DWORD_PTR)curProcInfo + curProcInfo->Size);
    }

    //
    // Loop the bitmap through the CPU activity array either horizontally or vertically
    // depending on dimensions of bitmap
    //
    while( TRUE ) {
        for( y = 0; y < (int)maxCpus / width; y++ ) {

            for( x = 0; x < width; x++ ) {
                if( bitmap.bmWidth > bitmap.bmHeight )
                    pixel = GetPixel( hSrcDC, (x + offset) % bitmap.bmWidth, y );
                else
                    pixel = GetPixel( hSrcDC, x, (y - offset) % bitmap.bmHeight );
                //printf("%06x ", pixel);
                greyPixel = RGB( GetRValue( pixel ), GetRValue( pixel ), GetRValue( pixel ) );
                CpuPixels[y * width + x] = 4 - greyPixel / (0xffffff / 4);
            }
            //printf("\n");
        }
        offset++;
        Sleep( 500 );
    }
}
