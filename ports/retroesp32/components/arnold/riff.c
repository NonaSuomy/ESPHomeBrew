/* 
 *  Arnold emulator (c) Copyright, Kevin Thacker 1995-2001
 *  GPL-2.0-or-later
 */

#include "cpcglob.h"
#include "riff.h"
#include "endian.h"

#include <stdint.h>

/* return length of chunk */
int Riff_GetChunkLength(RIFF_CHUNK *pChunk)
{
#ifdef CPC_LSB_FIRST
    return pChunk->ChunkLength;
#else
    return SwapEndianLong(pChunk->ChunkLength);
#endif
}

/* set length of a chunk */
void Riff_SetChunkLength(RIFF_CHUNK *pChunk, unsigned int Length)
{
#ifdef CPC_LSB_FIRST
    pChunk->ChunkLength = Length;
#else
    pChunk->ChunkLength = SwapEndianLong(Length);
#endif
}

/* return name of chunk */
unsigned int Riff_GetChunkName(RIFF_CHUNK *pChunk)
{
    return pChunk->ChunkName;
}

/* return pointer to chunk data */
unsigned char *Riff_GetChunkDataPtr(RIFF_CHUNK *pChunk)
{
    return (unsigned char *)((uintptr_t)pChunk + sizeof(RIFF_CHUNK));
}

/* get next chunk after chunk specified */
RIFF_CHUNK *Riff_GetNextChunk(RIFF_CHUNK *pChunk)
{
    return (RIFF_CHUNK *)((uintptr_t)pChunk + Riff_GetChunkLength(pChunk) + sizeof(RIFF_CHUNK));
}

/*
 * Given the pointer to a RIFF header chunk, search sub-chunks for ChunkName.
 * Returns pointer to matching chunk or NULL.
 */
RIFF_CHUNK *Riff_FindNamedSubChunk(RIFF_CHUNK *pHeader, unsigned int ChunkName)
{
    /* Data starts after RIFF header, then 4-byte RIFF type (e.g. "AMS!"). */
    RIFF_CHUNK *pChunk = (RIFF_CHUNK *)((uintptr_t)Riff_GetChunkDataPtr(pHeader) + 4);
    int ParentChunkLength = Riff_GetChunkLength(pHeader);

    do
    {
        if (pChunk->ChunkName == ChunkName)
        {
            return pChunk;
        }

        pChunk = Riff_GetNextChunk(pChunk);
    }
    while (((intptr_t)pChunk - (intptr_t)pHeader) < ParentChunkLength);

    return NULL;
}

/* current chunk is first chunk in RIFF file payload */
RIFF_CHUNK *Riff_GetFirstChunk(unsigned char *pFileStart)
{
    return (RIFF_CHUNK *)(pFileStart + sizeof(RIFF_CHUNK) + sizeof(unsigned int));
}

/* Validate that each chunk length stays within file bounds. */
BOOL Riff_CheckChunkSizesAreValid(unsigned char *pRiffFile, unsigned int RiffFileSize)
{
    unsigned int OffsetInFile;
    unsigned int ChunkLength;
    RIFF_CHUNK *pCurrentChunk = Riff_GetFirstChunk(pRiffFile);

    OffsetInFile = (unsigned int)((unsigned char *)pCurrentChunk - (unsigned char *)pRiffFile);

    do
    {
        ChunkLength = (unsigned int)Riff_GetChunkLength(pCurrentChunk);

        if (ChunkLength > (RiffFileSize - OffsetInFile))
        {
            return FALSE;
        }

        pCurrentChunk = Riff_GetNextChunk(pCurrentChunk);
        OffsetInFile = (unsigned int)((unsigned char *)pCurrentChunk - (unsigned char *)pRiffFile);
    }
    while (OffsetInFile < RiffFileSize);

    return TRUE;
}
