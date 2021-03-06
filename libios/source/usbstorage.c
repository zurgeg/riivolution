/*-------------------------------------------------------------

usbstorage.c -- Bulk-only USB mass storage support

Copyright (C) 2008
Sven Peter (svpe) <svpe@gmx.net>

This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any
damages arising from the use of this software.

Permission is granted to anyone to use this software for any
purpose, including commercial applications, and to alter it and
redistribute it freely, subject to the following restrictions:

1.	The origin of this software must not be misrepresented; you
must not claim that you wrote the original software. If you use
this software in a product, an acknowledgment in the product
documentation would be appreciated but is not required.

2.	Altered source versions must be plainly marked as such, and
must not be misrepresented as being the original software.

3.	This notice may not be removed or altered from any source
distribution.

-------------------------------------------------------------*/

#include <string.h>

#include "../include/usbstorage.h"
#include "../include/syscalls.h"
#include "../include/mem.h"
#include "../include/ipc.h"
#include "../include/timer.h"
#include "../include/gpio.h"

#define debug_printf(fmt, args...)
#define	TAG_START						0x0BADC0DE

#define	CBW_SIZE						31
#define	CBW_SIGNATURE					0x43425355
#define	CBW_IN							(1 << 7)
#define	CBW_OUT							0

#define	CSW_SIZE						13
#define	CSW_SIGNATURE					0x53425355

#define	SCSI_TEST_UNIT_READY			0x00
#define	SCSI_REQUEST_SENSE				0x03
#define SCSI_START_STOP					0x1B
#define	SCSI_READ_CAPACITY				0x25
#define	SCSI_READ_10					0x28
#define	SCSI_WRITE_10					0x2A

#define	SCSI_SENSE_REPLY_SIZE			18
#define	SCSI_SENSE_NOT_READY			0x02
#define	SCSI_SENSE_MEDIUM_ERROR			0x03
#define	SCSI_SENSE_HARDWARE_ERROR		0x04

#define	USB_CLASS_MASS_STORAGE			0x08
#define	MASS_STORAGE_SCSI_COMMANDS		0x06
#define	MASS_STORAGE_BULK_ONLY			0x50

#define USBSTORAGE_GET_MAX_LUN			0xFE
#define USBSTORAGE_RESET				0xFF

#define	USB_ENDPOINT_BULK				0x02

#define USBSTORAGE_CYCLE_RETRIES		3
#define USBSTORAGE_TIMEOUT				3

#define MAX_TRANSFER_SIZE				4096

#define DEVLIST_MAXSIZE    				8

/*
The following is for implementing a DISC_INTERFACE
as used by libfat
*/

#define DEVICE_TYPE_WII_USB (('W'<<24)|('U'<<16)|('S'<<8)|'B')

static usbstorage_handle __usbfd;
static u8 __lun = 0;
static u8 __mounted = 0;
static u16 __vid = 0;
static u16 __pid = 0;

static s32 __usbstorage_reset(usbstorage_handle *dev);
static s32 __usbstorage_clearerrors(usbstorage_handle *dev, u8 lun);

static s32 __USB_BlkMsgTimeout(usbstorage_handle *dev, u8 bEndpoint, u16 wLength, void *rpData, u32 timeout)
{
	s32 retval;

	retval = USB_WriteBlkMsg(dev->usbdev, bEndpoint, wLength, rpData, timeout);

	if(retval==USBSTORAGE_ETIMEDOUT)
	{
		gpio_set_on(GPIO_OSLOT);
		debug_printf("__USB_BlkMsgTimeout timed out, closing device\n");
		//USBStorage_Close(dev);
	}

	return retval;
}

static s32 __USB_CtrlMsgTimeout(usbstorage_handle *dev, u8 bmRequestType, u8 bmRequest, u16 wValue, u16 wIndex, u16 wLength, void *rpData)
{
	s32 retval;

	retval = USB_WriteCtrlMsg(dev->usbdev, bmRequestType, bmRequest, wValue, wIndex, wLength, rpData, USBSTORAGE_TIMEOUT);

	if(retval==USBSTORAGE_ETIMEDOUT)
	{
		gpio_set_on(GPIO_OSLOT);
		debug_printf("__USB_CtrlMsgTimeout timed out, closing device\n");
		//USBStorage_Close(dev);
	}

	return retval;
}

#define __stwbrx(base, index, value) *(u32*)(base+index) = SWAP32(value)
#define __lwbrx(base, index) SWAP32(*(u32*)(base+index))

static s32 __send_cbw(usbstorage_handle *dev, u8 lun, u32 len, u8 flags, const u8 *cb, u8 cbLen)
{
	s32 retval = USBSTORAGE_OK;

	if(cbLen == 0 || cbLen > 16)
		return IPC_EINVAL;

	memset(dev->buffer, 0, CBW_SIZE);

	++dev->tag;

	__stwbrx(dev->buffer, 0, CBW_SIGNATURE);
	__stwbrx(dev->buffer, 4, dev->tag);
	__stwbrx(dev->buffer, 8, len);
	dev->buffer[12] = flags;
	dev->buffer[13] = lun;
	dev->buffer[14] = (cbLen > 6) ? 10 : 6;

	memcpy(dev->buffer + 15, cb, cbLen);

	if(dev->suspended == 1)
	{
		debug_printf("Usbstorage: Resuming device\n");
		USB_ResumeDevice(dev->usbdev);
		dev->suspended = 0;
	}

	retval = __USB_BlkMsgTimeout(dev, dev->ep_out, CBW_SIZE, (void *)dev->buffer, USBSTORAGE_TIMEOUT);

	if(retval == CBW_SIZE) return USBSTORAGE_OK;
	else if(retval > 0) return USBSTORAGE_ESHORTWRITE;

	return retval;
}

static s32 __read_csw(usbstorage_handle *dev, u8 *status, u32 *dataResidue, u32 timeout)
{
	s32 retval = USBSTORAGE_OK;
	u32 signature, tag, _dataResidue, _status;

	memset(dev->buffer, 0, CSW_SIZE);

	retval = __USB_BlkMsgTimeout(dev, dev->ep_in, CSW_SIZE, dev->buffer, timeout);
	if(retval > 0 && retval != CSW_SIZE) return USBSTORAGE_ESHORTREAD;
	else if(retval < 0) return retval;

	signature = __lwbrx(dev->buffer, 0);
	tag = __lwbrx(dev->buffer, 4);
	_dataResidue = __lwbrx(dev->buffer, 8);
	_status = dev->buffer[12];

	if(signature != CSW_SIGNATURE) return USBSTORAGE_ESIGNATURE;
	if(tag != dev->tag) return USBSTORAGE_ETAG;

	if(dataResidue != NULL)
		*dataResidue = _dataResidue;
	if(status != NULL)
		*status = _status;


	return USBSTORAGE_OK;
}

static s32 __cycle(usbstorage_handle *dev, u8 lun, u8 *buffer, u32 len, u8 *cb, u8 cbLen, u8 write, u8 *_status, u32 *_dataResidue)
{
	s32 retval = USBSTORAGE_OK;

	u8 status = 0;
	u32 dataResidue = 0;
	u32 thisLen;

	s8 retries = USBSTORAGE_CYCLE_RETRIES + 1;

	do
	{
		retries--;

		if(retval == USBSTORAGE_ETIMEDOUT)
			break;

		if(write)
		{
			retval = __send_cbw(dev, lun, len, CBW_OUT, cb, cbLen);
			if(retval == USBSTORAGE_ETIMEDOUT)
				break;
			if(retval < 0)
			{
				if(__usbstorage_reset(dev) == USBSTORAGE_ETIMEDOUT)
					retval = USBSTORAGE_ETIMEDOUT;
				continue;
			}
			while(len > 0)
			{
				thisLen = len > MAX_TRANSFER_SIZE ? MAX_TRANSFER_SIZE : len;
				if ((u32)buffer & 0x1F || (u32)buffer < 0x10000000) {
					memcpy(dev->buffer, buffer, thisLen);
					retval = __USB_BlkMsgTimeout(dev, dev->ep_out, thisLen, dev->buffer, USBSTORAGE_TIMEOUT);
				} else
					retval = __USB_BlkMsgTimeout(dev, dev->ep_out, thisLen, buffer, USBSTORAGE_TIMEOUT);


				if(retval == USBSTORAGE_ETIMEDOUT)
					break;

				if(retval < 0)
				{
					retval = USBSTORAGE_EDATARESIDUE;
					break;
				}


				if(retval != thisLen && len > 0)
				{
					retval = USBSTORAGE_EDATARESIDUE;
					break;
				}
				len -= retval;
				buffer += retval;
			}

			if(retval < 0)
			{
				if(__usbstorage_reset(dev) == USBSTORAGE_ETIMEDOUT)
					retval = USBSTORAGE_ETIMEDOUT;
				continue;
			}
		}
		else
		{
			retval = __send_cbw(dev, lun, len, CBW_IN, cb, cbLen);

			if(retval == USBSTORAGE_ETIMEDOUT)
				break;

			if(retval < 0)
			{
				if(__usbstorage_reset(dev) == USBSTORAGE_ETIMEDOUT)
					retval = USBSTORAGE_ETIMEDOUT;
				continue;
			}
			while(len > 0)
			{
				thisLen = len > MAX_TRANSFER_SIZE ? MAX_TRANSFER_SIZE : len;
				if ((u32)buffer & 0x1F || (u32)buffer < 0x10000000) {
					retval = __USB_BlkMsgTimeout(dev, dev->ep_in, thisLen, dev->buffer, USBSTORAGE_TIMEOUT);
					if (retval>0)
						memcpy(buffer, dev->buffer, retval);
				} else
					retval = __USB_BlkMsgTimeout(dev, dev->ep_in, thisLen, buffer, USBSTORAGE_TIMEOUT);

				if(retval < 0)
					break;

				len -= retval;
				buffer += retval;

				if(retval != thisLen)
					break;
			}

			if(retval < 0)
			{
				if(__usbstorage_reset(dev) == USBSTORAGE_ETIMEDOUT)
					retval = USBSTORAGE_ETIMEDOUT;
				continue;
			}
		}

		retval = __read_csw(dev, &status, &dataResidue, USBSTORAGE_TIMEOUT);

		if(retval == USBSTORAGE_ETIMEDOUT)
			break;

		if(retval < 0)
		{
			if(__usbstorage_reset(dev) == USBSTORAGE_ETIMEDOUT)
				retval = USBSTORAGE_ETIMEDOUT;
			continue;
		}

		retval = USBSTORAGE_OK;
	} while(retval < 0 && retries > 0);

	if(retval < 0 && retval != USBSTORAGE_ETIMEDOUT)
	{
		if(__usbstorage_reset(dev) == USBSTORAGE_ETIMEDOUT)
			retval = USBSTORAGE_ETIMEDOUT;
	}

	if(_status != NULL)
		*_status = status;
	if(_dataResidue != NULL)
		*_dataResidue = dataResidue;

	return retval;
}

static s32 __usbstorage_clearerrors(usbstorage_handle *dev, u8 lun)
{
	s32 retval;
	u8 cmd[16];
	u8 sense[SCSI_SENSE_REPLY_SIZE];
	u8 status = 0;

	memset(cmd, 0, sizeof(cmd));
	cmd[0] = SCSI_TEST_UNIT_READY;

	retval = __cycle(dev, lun, NULL, 0, cmd, 6, 0, &status, NULL);
	if(retval < 0)
		return retval;

	if (status != 0)
	{
		cmd[0] = SCSI_REQUEST_SENSE;
		cmd[1] = lun << 5;
		cmd[4] = SCSI_SENSE_REPLY_SIZE;
		cmd[5] = 0;
		memset(sense, 0, SCSI_SENSE_REPLY_SIZE);
		retval = __cycle(dev, lun, sense, SCSI_SENSE_REPLY_SIZE, cmd, 6, 0, NULL, NULL);
		if(retval < 0)
			return retval;

		status = sense[2] & 0x0F;
		switch (status) {
			case SCSI_SENSE_NOT_READY:
			case SCSI_SENSE_MEDIUM_ERROR:
			case SCSI_SENSE_HARDWARE_ERROR:
				return USBSTORAGE_ESENSE;
		}
	}

	return retval;
}

static s32 __usbstorage_reset(usbstorage_handle *dev)
{
	s32 retval;

	if(dev->suspended == 1)
	{
		USB_ResumeDevice(dev->usbdev);
		dev->suspended = 0;
	}

	retval = __USB_CtrlMsgTimeout(dev, (USB_CTRLTYPE_DIR_HOST2DEVICE | USB_CTRLTYPE_TYPE_CLASS | USB_CTRLTYPE_REC_INTERFACE), USBSTORAGE_RESET, 0, dev->interface, 0, NULL);

	/* gives device enough time to process the reset */
	usleep(100);

	return retval;
}

s32 USBStorage_Open(usbstorage_handle *dev, const char *bus, u16 vid, u16 pid)
{
	s32 retval = -1;
	u8 conf,*max_lun = NULL;
	u32 iConf, iInterface, iEp;
	usb_devdesc udd;
	usb_configurationdesc *ucd;
	usb_interfacedesc *uid;
	usb_endpointdesc *ued;

	max_lun = Memalign(32, 1);
	if(max_lun==NULL) return IPC_ENOMEM;

	memset(dev, 0, sizeof(*dev));

	dev->tag = TAG_START;

	dev->usbdev = USB_OpenDevice(bus, vid, pid);
	if(dev->usbdev==NULL)
	{
		debug_printf("Failed to open USB device\n");
		goto free_and_return;
	}

	USB_ResumeDevice(dev->usbdev);

	retval = USB_GetDescriptors(dev->usbdev, &udd);
	if(retval < 0)
	{
		debug_printf("Failed to get USB descriptors\n");
		goto free_and_return;
	}

	for(iConf = 0; iConf < udd.bNumConfigurations; iConf++)
	{
		ucd = &udd.configurations[iConf];
		for(iInterface = 0; iInterface < ucd->bNumInterfaces; iInterface++)
		{
			uid = &ucd->interfaces[iInterface];
			if(uid->bInterfaceClass    == USB_CLASS_MASS_STORAGE &&
			   uid->bInterfaceSubClass == MASS_STORAGE_SCSI_COMMANDS &&
			   uid->bInterfaceProtocol == MASS_STORAGE_BULK_ONLY)
			{
				if(uid->bNumEndpoints < 2)
					continue;

				dev->ep_in = dev->ep_out = 0;
				for(iEp = 0; iEp < uid->bNumEndpoints; iEp++)
				{
					ued = &uid->endpoints[iEp];
					if(ued->bmAttributes != USB_ENDPOINT_BULK)
						continue;

					if(ued->bEndpointAddress & USB_ENDPOINT_IN)
						dev->ep_in = ued->bEndpointAddress;
					else
						dev->ep_out = ued->bEndpointAddress;
				}
				if(dev->ep_in != 0 && dev->ep_out != 0)
				{
					dev->configuration = ucd->bConfigurationValue;
					dev->interface = uid->bInterfaceNumber;
					dev->altInterface = uid->bAlternateSetting;
					goto found;
				}
			}
		}
	}

	USB_FreeDescriptors(&udd);
	debug_printf("USB mass storage interface not found\n");
	retval = USBSTORAGE_ENOINTERFACE;
	goto free_and_return;

found:
	USB_FreeDescriptors(&udd);
	debug_printf("Found USB Mass storage device\n");

	retval = USBSTORAGE_EINIT;
	conf = 0;
	USB_GetConfiguration(dev->usbdev, &conf); // don't care about the result
	if (conf != dev->configuration && USB_SetConfiguration(dev->usbdev, dev->configuration) < 0) {
		debug_printf("USB_SetConfiguration failed\n");
		goto free_and_return;
	}
	if(dev->altInterface != 0 && USB_SetAlternativeInterface(dev->usbdev, dev->interface, dev->altInterface) < 0)
		goto free_and_return;
	dev->suspended = 0;

	retval = USBStorage_Reset(dev);
	if(retval < 0)
	{
		debug_printf("USBStorage_Reset failed\n");
		goto free_and_return;
	}

	retval = __USB_CtrlMsgTimeout(dev, (USB_CTRLTYPE_DIR_DEVICE2HOST | USB_CTRLTYPE_TYPE_CLASS | USB_CTRLTYPE_REC_INTERFACE), USBSTORAGE_GET_MAX_LUN, 0, dev->interface, 1, max_lun);
	if(retval < 0)
		*max_lun = 0; // doesn't support multiple LUNs, may be STALLed

	dev->max_lun = *max_lun+1;

	if(retval == USBSTORAGE_ETIMEDOUT)
		goto free_and_return;

	retval = USBSTORAGE_OK;

	dev->sector_size = Alloc(dev->max_lun*sizeof(u32));
	if(dev->sector_size == NULL)
	{
		retval = IPC_ENOMEM;
		goto free_and_return;
	}
	memset(dev->sector_size, 0, dev->max_lun*sizeof(u32));

	/* taken from linux usbstorage module (drivers/usb/storage/transport.c) */
	/*
	 * Some devices (i.e. Iomega Zip100) need this -- apparently
	 * the bulk pipes get STALLed when the GetMaxLUN request is
	 * processed.   This is, in theory, harmless to all other devices
	 * (regardless of if they stall or not).
	 */
	//USB_ClearHalt(dev->usbdev, dev->ep_in);
	//USB_ClearHalt(dev->usbdev, dev->ep_out);

	dev->buffer = Memalign(32, MAX_TRANSFER_SIZE);

	if(dev->buffer == NULL) retval = IPC_ENOMEM;
	else {
		// FIXME
		//USB_DeviceRemovalNotifyAsync(dev->usb_fd,__usb_deviceremoved_cb,dev);
		retval = USBSTORAGE_OK;
	}

free_and_return:
	Dealloc(max_lun);
	if(retval < 0)
	{
		USB_CloseDevice(dev->usbdev);
		Dealloc(dev->buffer);
		Dealloc(dev->sector_size);
		memset(dev, 0, sizeof(*dev));
		return retval;
	}
	return 0;
}

s32 USBStorage_Close(usbstorage_handle *dev)
{
	if (dev->usbdev)
		USB_CloseDevice(dev->usbdev);
	Dealloc(dev->sector_size);
	Dealloc(dev->buffer);
	memset(dev, 0, sizeof(*dev));
    __lun = 0;
    __vid = 0;
    __pid = 0;

	return 0;
}

s32 USBStorage_Reset(usbstorage_handle *dev)
{
	return __usbstorage_reset(dev);
}

s32 USBStorage_GetMaxLUN(usbstorage_handle *dev)
{
	return dev->max_lun;
}

s32 USBStorage_MountLUN(usbstorage_handle *dev, u8 lun)
{
	s32 retval=0;
	int retries;

	if(lun >= dev->max_lun)
		return IPC_EINVAL;

	for (retries=0; retries<2; retries++) {
		retval = __usbstorage_clearerrors(dev, lun);
		if (retval==USBSTORAGE_OK)
			break;
		usleep(1000);
	}

	if(retval < 0) {
		debug_printf("MountLUN: Couldn't clear errors (%d)\n", retval);
		return retval;
	}

	retval = USBStorage_ReadCapacity(dev, lun, &dev->sector_size[lun], NULL);
	return retval;
}

s32 USBStorage_ReadCapacity(usbstorage_handle *dev, u8 lun, u32 *sector_size, u32 *n_sectors)
{
	s32 retval;
	u8 cmd[10] = {SCSI_READ_CAPACITY, lun<<5, 0,0,0,0,0,0,0,0};
	u8 response[8];
	u8 status;

	memset(response, 0, sizeof(response));
	retval = __cycle(dev, lun, response, sizeof(response), cmd, sizeof(cmd), 0, &status, NULL);
	if (status)
		retval = USBSTORAGE_ESHORTREAD;
	if(retval >= 0)
	{
		u32 _n_sectors, _sector_size;
		memcpy(&_n_sectors, response, 4);
		memcpy(&_sector_size, response+4, 4);
		debug_printf("Number of sectors: %u, Sector size %u (%08X)\n", _n_sectors, _sector_size, retval);
		if(n_sectors != NULL)
			*n_sectors = _n_sectors;
		if(sector_size != NULL)
			*sector_size = _sector_size;
		retval = USBSTORAGE_OK;
	} else
		debug_printf("ReadCapacity failed %d\n", retval);

	return retval;
}

s32 USBStorage_StartStop(usbstorage_handle *dev, u8 lun, u8 lo_ej, u8 start, u8 imm)
{
	u8 status = 0;
	s32 retval = USBSTORAGE_OK;
	u8 cmd[] = {
		SCSI_START_STOP,
		(lun << 5) | (imm&1),
		0,
		0,
		((lo_ej&1)<<1) | (start&1),
		0
	};

	if(lun >= dev->max_lun)
		return IPC_EINVAL;

	retval = __send_cbw(dev, lun, 0, CBW_IN, cmd, sizeof(cmd));

	// if imm==0, wait up to 10secs for spinup to finish
	if (retval >= 0)
		retval = __read_csw(dev, &status, NULL, (imm ? USBSTORAGE_TIMEOUT : 10));

	if(retval >=0 && status != 0)
		retval = USBSTORAGE_ESTATUS;

	return retval;
}

s32 USBStorage_Read(usbstorage_handle *dev, u8 lun, u32 sector, u16 n_sectors, u8 *buffer)
{
	u8 status = 0;
	s32 retval;

	if(lun >= dev->max_lun)
		return IPC_EINVAL;

	u8 cmd[10] = {
		SCSI_READ_10,
		lun << 5,
		sector >> 24,
		sector >> 16,
		sector >>  8,
		sector,
		0,
		n_sectors >> 8,
		n_sectors,
		0
		};

	USBStorage_StartStop(dev, lun, 0, 1, 0);
	retval = __cycle(dev, lun, buffer, n_sectors * dev->sector_size[lun], cmd, sizeof(cmd), 0, &status, NULL);
	if(retval > 0 && status != 0)
		retval = USBSTORAGE_ESTATUS;
	return retval;
}

s32 USBStorage_Write(usbstorage_handle *dev, u8 lun, u32 sector, u16 n_sectors, const u8 *buffer)
{
	u8 status = 0;
	s32 retval;
	if(lun >= dev->max_lun)
		return IPC_EINVAL;

	u8 cmd[10] = {
		SCSI_WRITE_10,
		lun << 5,
		sector >> 24,
		sector >> 16,
		sector >> 8,
		sector,
		0,
		n_sectors >> 8,
		n_sectors,
		0
		};

	USBStorage_StartStop(dev, lun, 0, 1, 0);
	retval = __cycle(dev, lun, (u8 *)buffer, n_sectors * dev->sector_size[lun], cmd, sizeof(cmd), 1, &status, NULL);
	if(retval > 0 && status != 0)
		retval = USBSTORAGE_ESTATUS;
	return retval;
}

s32 USBStorage_Suspend(usbstorage_handle *dev)
{
	if(dev->suspended == 1)
		return USBSTORAGE_OK;

	USB_SuspendDevice(dev->usbdev);
	dev->suspended = 1;

	return USBSTORAGE_OK;

}

/*
The following is for implementing a DISC_INTERFACE
as used by libfat
*/

static bool __usbstorage_ReadSectors(u32, u32, void *);

static bool __usbstorage_IsInserted(void);

static bool __usbstorage_Startup(void)
{
	return __usbstorage_IsInserted();
}

static bool __usbstorage_IsInserted(void)
{
   u8 *buffer;
   u8 i, cnt_device;
   int j;
   u16 vid, pid;
   s32 maxLun;
   s32 retval;

   __mounted = 0;		//reset it here and check if device is still attached

   buffer = Memalign(32, DEVLIST_MAXSIZE << 3);
   if(buffer == NULL)
       return false;
   memset(buffer, 0, DEVLIST_MAXSIZE << 3);

   if(USB_GetDeviceList("/dev/usb/oh0", buffer, DEVLIST_MAXSIZE, 8, &cnt_device) < 0)
   {
	   debug_printf("Couldn't get USB device list\n");
		if(__vid!=0 || __pid!=0)
			USBStorage_Close(&__usbfd);

       __lun = 0;
       __vid = 0;
       __pid = 0;
       Dealloc(buffer);
       return false;
   }

   debug_printf("Found %d USB Mass Storage devices\n", cnt_device);
   usleep(100);

   if(__vid!=0 || __pid!=0)
   {
       for(i = 0; i < cnt_device; i++)
       {
           memcpy(&vid, (buffer + (i << 3) + 4), 2);
           memcpy(&pid, (buffer + (i << 3) + 6), 2);
           if(vid != 0 || pid != 0)
           {
               if( (vid == __vid) && (pid == __pid) && USBStorage_ReadCapacity(&__usbfd, __lun, NULL, NULL)==USBSTORAGE_OK)
               {
                   __mounted = 1;
                   Dealloc(buffer);
                   usleep(50); // I don't know why I have to wait but it's needed
                   return true;
               }
			   debug_printf("This isn't the device you're looking for %04X %04X.\n", vid, pid);
           }
       }
	   debug_printf("Couldn't find existing device, was it removed?\n");
	   USBStorage_Close(&__usbfd);
   }

   __lun = 0;
   __vid = 0;
   __pid = 0;

   for(i = 0; i < cnt_device; i++)
   {
       memcpy(&vid, (buffer + (i << 3) + 4), 2);
       memcpy(&pid, (buffer + (i << 3) + 6), 2);
       if(vid == 0 || pid == 0)
           continue;

       if(USBStorage_Open(&__usbfd, "oh0", vid, pid) < 0)
           continue;

       maxLun = USBStorage_GetMaxLUN(&__usbfd);
	   debug_printf("Storage device maxLUN is %d\n", maxLun-1);
       for(j = maxLun-1; j >=0; j--)
       {
           USBStorage_StartStop(&__usbfd, j, 0, 1, 0);
		   debug_printf("Trying to mount LUN %d\n", j);
           retval = USBStorage_MountLUN(&__usbfd, j);

           if(retval == USBSTORAGE_ETIMEDOUT) {
			   debug_printf("Timed out trying to mount LUN %d\n", j);
               break;
		   }
           if(retval < 0) {
			   debug_printf("Error trying to mount LUN %d\n", j);
               continue;
		   }

           __mounted = 1;
           __lun = j;
           __vid = vid;
           __pid = pid;
           i = cnt_device;
           break;
       }

       if (!__mounted)
           USBStorage_Close(&__usbfd);
   }
   Dealloc(buffer);
   if(__mounted == 1)
       return true;
   return false;
}

static bool __usbstorage_ReadSectors(u32 sector, u32 numSectors, void *buffer)
{
   s32 retval;

   if(__mounted != 1)
       return false;

	gpio_set_toggle(GPIO_OSLOT);

   retval = USBStorage_Read(&__usbfd, __lun, sector, numSectors, buffer);

	gpio_set_toggle(GPIO_OSLOT);

   if(retval == USBSTORAGE_ETIMEDOUT)
       __mounted = 0;

   if(retval < 0)
       return false;

   return true;
}

static bool __usbstorage_WriteSectors(u32 sector, u32 numSectors, const void *buffer)
{
   s32 retval;

   if(__mounted != 1)
       return false;

	gpio_set_toggle(GPIO_OSLOT);

   retval = USBStorage_Write(&__usbfd, __lun, sector, numSectors, buffer);

	gpio_set_toggle(GPIO_OSLOT);

   if(retval == USBSTORAGE_ETIMEDOUT)
       __mounted = 0;

   if(retval < 0)
       return false;
   return true;
}

static bool __usbstorage_ClearStatus(void)
{
   return true;
}

static bool __usbstorage_Shutdown(void)
{
	if(__mounted)
		USBStorage_Close(&__usbfd);

   __mounted = 0;
   return true;
}

DISC_INTERFACE __io_usbstorage = {
   DEVICE_TYPE_WII_USB,
   FEATURE_MEDIUM_CANREAD | FEATURE_MEDIUM_CANWRITE | FEATURE_WII_USB,
   (FN_MEDIUM_STARTUP)&__usbstorage_Startup,
   (FN_MEDIUM_ISINSERTED)&__usbstorage_IsInserted,
   (FN_MEDIUM_READSECTORS)&__usbstorage_ReadSectors,
   (FN_MEDIUM_WRITESECTORS)&__usbstorage_WriteSectors,
   (FN_MEDIUM_CLEARSTATUS)&__usbstorage_ClearStatus,
   (FN_MEDIUM_SHUTDOWN)&__usbstorage_Shutdown
};

