//
// usbmidi.c
//
// USPi - An USB driver for Raspberry Pi written in C
// Copyright (C) 2016  R. Stange <rsta2@o2online.de>
// Copyright (C) 2016  J. Otto <joshua.t.otto@gmail.com>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

// Refer to "Universal Serial Bus Device Class Specification for MIDI Devices"

#include <uspi/usbmidi.h>
#include <uspi/usbhostcontroller.h>
#include <uspi/devicenameservice.h>
#include <uspi/assert.h>
#include <uspios.h>

static const char FromMIDI[] = "umidi";

static unsigned s_nDeviceNumber = 1;

static boolean USBMIDIDeviceStartRequest (TUSBMIDIDevice *pThis);
static void USBMIDIDeviceCompletionRoutine (TUSBRequest *pURB, void *pParam, void *pContext);

// This handy table from the Linux driver encodes the mapping between MIDI
// packet Code Index Number values and encapsulated packet lengths.
static const unsigned cin_to_length[] = {
	0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1
};

#define EVENT_PACKET_SIZE 4

void USBMIDIDevice (TUSBMIDIDevice *pThis, TUSBDevice *pDevice)
{
	assert (pThis != 0);

	USBDeviceCopy (&pThis->m_USBDevice, pDevice);
	pThis->m_USBDevice.Configure = USBMIDIDeviceConfigure;

	pThis->m_pEndpointIn = 0;
	pThis->m_pPacketHandler = 0;
	pThis->m_pURB = 0;
	pThis->m_pPacketBuffer = 0;
}

void _CUSBMIDIDevice (TUSBMIDIDevice *pThis)
{
	assert (pThis != 0);

	if (pThis->m_pPacketBuffer != 0)
	{
		free (pThis->m_pPacketBuffer);
		pThis->m_pPacketBuffer = 0;
	}

	if (pThis->m_pEndpointIn != 0)
	{
		_USBEndpoint (pThis->m_pEndpointIn);
		free (pThis->m_pEndpointIn);
		pThis->m_pEndpointIn = 0;
	}

	_USBDevice (&pThis->m_USBDevice);
}

boolean USBMIDIDeviceConfigure (TUSBDevice *pUSBDevice)
{
	TUSBMIDIDevice *pThis = (TUSBMIDIDevice *)pUSBDevice;
	assert (pThis != 0);

	TUSBConfigurationDescriptor *pConfDesc =
		(TUSBConfigurationDescriptor *) USBDeviceGetDescriptor (&pThis->m_USBDevice, DESCRIPTOR_CONFIGURATION);
	if (   pConfDesc == 0
		|| pConfDesc->bNumInterfaces < 1)
	{
		USBDeviceConfigurationError (&pThis->m_USBDevice, FromMIDI);

		return FALSE;
	}

	TString *pName = USBDeviceGetName (pUSBDevice, DeviceNameVendor);
	boolean bSpecialRolandSnowflake = StringCompare (pName, "ven582-12a") == 0;
	if (bSpecialRolandSnowflake)
	{
		LogWrite (FromMIDI, LOG_NOTICE, "Roland UM-ONE detected");
	}
	_String (pName);
	free (pName);

	TUSBInterfaceDescriptor *pInterfaceDesc;
	while ((pInterfaceDesc = (TUSBInterfaceDescriptor *) USBDeviceGetDescriptor (&pThis->m_USBDevice, DESCRIPTOR_INTERFACE)) != 0)
	{
		if (   pInterfaceDesc->bNumEndpoints		==  0
			|| pInterfaceDesc->bInterfaceClass		!= (bSpecialRolandSnowflake ? 0xff : 0x01)
			|| pInterfaceDesc->bInterfaceSubClass	!= 0x03  // MIDI streaming
			|| pInterfaceDesc->bInterfaceProtocol	!= 0x00) // unused, must be 0
		{
			continue;
		}

		// Our strategy for now is simple: we'll take the first MIDI streaming
		// bulk-in endpoint on this interface we can find.  To distinguish
		// between the MIDI streaming bulk-in endpoints we want (which carry
		// actual MIDI data streams) and 'transfer bulk data' endpoints (which
		// are used to implement features like Downloadable Samples that we
		// don't care about), we'll look for an immediately-accompanying
		// class-specific endpoint descriptor.
		TUSBAudioEndpointDescriptor *pEndpointDesc;
		while ((pEndpointDesc = (TUSBAudioEndpointDescriptor *) USBDeviceGetDescriptor (&pThis->m_USBDevice, DESCRIPTOR_ENDPOINT)) != 0)
		{
			if (   (pEndpointDesc->bEndpointAddress & 0x80) != 0x80		// Input EP
				|| (pEndpointDesc->bmAttributes     & 0x3F) != 0x02)	// Bulk EP
			{
				continue;
			}

			if (!bSpecialRolandSnowflake)
			{
				TUSBMIDIStreamingEndpointDescriptor *pMIDIDesc =
					(TUSBMIDIStreamingEndpointDescriptor *) USBDeviceGetDescriptor (&pThis->m_USBDevice, DESCRIPTOR_CS_ENDPOINT);
				if (   pMIDIDesc == 0
					|| (u8 *)pEndpointDesc + pEndpointDesc->bLength != (u8 *)pMIDIDesc)
				{
					continue;
				}
			}

			assert (pThis->m_pEndpointIn == 0);
			pThis->m_pEndpointIn = malloc (sizeof (TUSBEndpoint));
			assert (pThis->m_pEndpointIn != 0);

			pThis->m_usBufferSize = pEndpointDesc->wMaxPacketSize;
			pThis->m_usBufferSize -= pEndpointDesc->wMaxPacketSize % EVENT_PACKET_SIZE;
			assert (pThis->m_pPacketBuffer == 0);
			pThis->m_pPacketBuffer = malloc (pThis->m_usBufferSize);
			assert (pThis->m_pPacketBuffer != 0);

			USBEndpoint2 (pThis->m_pEndpointIn, &pThis->m_USBDevice, (TUSBEndpointDescriptor *) pEndpointDesc);
		}
	}

	if (pThis->m_pEndpointIn == 0)
	{
		USBDeviceConfigurationError (&pThis->m_USBDevice, FromMIDI);

		return FALSE;
	}

	if (!USBDeviceConfigure (&pThis->m_USBDevice))
	{
		LogWrite (FromMIDI, LOG_ERROR, "Cannot set configuration");

		return FALSE;
	}

	TString DeviceName;
	String (&DeviceName);
	StringFormat (&DeviceName, "umidi%u", s_nDeviceNumber++);
	DeviceNameServiceAddDevice (DeviceNameServiceGet (), StringGet (&DeviceName), pThis, FALSE);

	_String(&DeviceName);

	return USBMIDIDeviceStartRequest (pThis);
}

void USBMIDIDeviceRegisterPacketHandler (TUSBMIDIDevice *pThis, TMIDIPacketHandler *pPacketHandler)
{
	assert (pThis != 0);
	assert (pPacketHandler != 0);
	pThis->m_pPacketHandler = pPacketHandler;
}

boolean USBMIDIDeviceStartRequest (TUSBMIDIDevice *pThis)
{
	assert (pThis != 0);

	assert (pThis->m_pEndpointIn != 0);
	assert (pThis->m_pPacketBuffer != 0);

	assert (pThis->m_pURB == 0);
	pThis->m_pURB = malloc (sizeof (TUSBRequest));
	assert (pThis->m_pURB != 0);
	USBRequest (pThis->m_pURB, pThis->m_pEndpointIn, pThis->m_pPacketBuffer, pThis->m_usBufferSize, 0);
	USBRequestSetCompletionRoutine (pThis->m_pURB, USBMIDIDeviceCompletionRoutine, 0, pThis);

	return DWHCIDeviceSubmitAsyncRequest (USBDeviceGetHost (&pThis->m_USBDevice), pThis->m_pURB);
}

void USBMIDIDeviceCompletionRoutine (TUSBRequest *pURB, void *pParam, void *pContext)
{
	TUSBMIDIDevice *pThis = (TUSBMIDIDevice *) pContext;
	assert (pThis != 0);

	assert (pURB != 0);
	assert (pThis->m_pURB == pURB);

	if (   USBRequestGetStatus (pURB) != 0
		&& USBRequestGetResultLength (pURB) % EVENT_PACKET_SIZE == 0)
	{
		assert (pThis->m_pPacketBuffer != 0);

		// XXX HEY
		//
		// We're changing the library behaviour here.  In my application I need
		// to signal the receipt of MIDI data to another core via an IPI, and
		// I'd really rather just generate one IPI for all of the currently
		// avaialble MIDI data.  I'm going to abuse the existing MIDI callback
		// infrastructure to do what I want.
		if (pThis->m_pPacketHandler != 0)
		{
			unsigned nLength = USBRequestGetResultLength(pURB);
			pThis->m_pPacketHandler(0, nLength, pThis->m_pPacketBuffer);
		}
	}

	_USBRequest (pThis->m_pURB);
	free (pThis->m_pURB);
	pThis->m_pURB = 0;

	USBMIDIDeviceStartRequest (pThis);
}
