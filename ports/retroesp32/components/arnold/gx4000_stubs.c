#include "cpcglob.h"
#include "device.h"
#include "yiq.h"
#include "z8536.h"

/* Cartridge-only target: these peripherals are intentionally stubbed. */

/* Cassette */
unsigned long Cassette_Read(unsigned long NopsPassed)
{
	(void)NopsPassed;
	return 0;
}

void Cassette_Write(unsigned long NopsPassed, unsigned long State)
{
	(void)NopsPassed;
	(void)State;
}

void Cassette_Init(void) {}
void Cassette_Finish(void) {}
void Tape_Remove(void) {}

/* Brightness */
void BrightnessControl_Initialise(void) {}

void BrightnessControl_GenerateGreyScaleFromColour(RGB_CHAR *pSourceRGB, RGB_CHAR *pDestRGB)
{
	if (pSourceRGB == NULL || pDestRGB == NULL)
	{
		return;
	}
	*pDestRGB = *pSourceRGB;
}

/* FDD/FDC/FDI */
void FDD_InitialiseAll(void) {}
void FDD_Initialise(int Drive) { (void)Drive; }
void FDC_Reset(void) {}
void FDI_SetMotorState(int State) { (void)State; }
void FDC_WriteDataRegister(int Data) { (void)Data; }

unsigned int FDC_ReadDataRegister(void)
{
	return 0xFFu;
}

unsigned int FDC_ReadMainStatusRegister(void)
{
	return 0x80u;
}

/* Disk images */
void DiskImage_Initialise(void) {}
void DiskImage_Finish(void) {}

/* Printer */
void Printer_WriteDataByte(int Data) { (void)Data; }
void Printer_SetDataBit7State(int State) { (void)State; }

/* Z8536 */
Z8536 z8536;

void Z8536_Update(void) {}

void Z8536_WriteData(unsigned char Address, unsigned char Data)
{
	(void)Address;
	(void)Data;
}

unsigned char Z8536_ReadData(unsigned char Address)
{
	(void)Address;
	return 0xFFu;
}

/* Optional helpers referenced by some core configs */
void CPC_SetCassetteType(int Type) { (void)Type; }
unsigned long CPC_GetCassetteType(void) { return 0; }
