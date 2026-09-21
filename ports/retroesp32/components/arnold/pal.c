#include <stdlib.h>
#pragma GCC optimize ("Ofast")
#include "pal.h"
#include "garray.h"

PAL16L8 *PAL_Context = NULL;

extern unsigned char **RamConfigurationTable;

static int PAL_EnsureAllocated(void)
{
    if (!PAL_Context)
        PAL_Context = calloc(1, sizeof(*PAL_Context));

    return PAL_Context != NULL;
}

void PAL_Initialise(void)
{
    if (!PAL_EnsureAllocated())
        return;

    PAL_Context->pChosenRamConfig = RamConfigurationTable ? &RamConfigurationTable[0] : NULL;
}

void PAL_Finish(void)
{
    free(PAL_Context);
    PAL_Context = NULL;
}

void PAL_Reset(void)
{
	if (!PAL_EnsureAllocated())
		return;

	PAL_WriteConfig(0x0c0);
}

int  PAL_GetRamConfiguration(void)
{
    return PAL_Context ? PAL_Context->RamConfig : 0;
}


void	PAL_WriteConfig(int Function)
{
	if (!PAL_EnsureAllocated() || !RamConfigurationTable)
		return;

	if ((Function & 0x0c0)==0x0c0)
	{
        /* function 11xxxxxx */
        int     Config;

        PAL_Context->RamConfig = (unsigned char)Function;

        Config = Function & 0x03f;
        Config = Config<<2;

        PAL_Context->pChosenRamConfig = &RamConfigurationTable[Config];
		
		GateArray_RethinkMemory();
	}
}
