/*
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * Copyright (c) 1998-2003 Apple Computer, Inc.  All Rights Reserved.
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */

#include <machine/limits.h>			/* UINT_MAX */
#include <libkern/OSByteOrder.h>

#include <IOKit/network/IOEthernetController.h>
#include <IOKit/network/IOEthernetInterface.h>
#include <IOKit/network/IOGatedOutputQueue.h>

#include <IOKit/IOTimerEventSource.h>
#include <IOKit/assert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMessage.h>

#include <IOKit/pwr_mgt/RootDomain.h>

#include <IOKit/usb/IOUSBBus.h>
#include <IOKit/usb/IOUSBNub.h>
#include <IOKit/usb/IOUSBDevice.h>
#include <IOKit/usb/IOUSBPipe.h>
#include <IOKit/usb/USB.h>
#include <IOKit/usb/IOUSBInterface.h>

#include <UserNotification/KUNCUserNotifications.h>

extern "C"
{
    #include <sys/param.h>
    #include <sys/mbuf.h>
}

#define DEBUG_NAME "AppleUSBCDCEEM"
 
#include "AppleUSBCDCEEM.h"

#define MIN_BAUD (50 << 1)

#if USE_ELG
    com_apple_iokit_XTrace	*gXTrace = 0;
#endif

//AppleUSBCDCEEMControl		*gControlDriver = NULL;			// Our Control driver
    
static struct MediumTable
{
    UInt32	type;
    UInt32	speed;
}

mediumTable[] =
{
    {kIOMediumEthernetNone,												0},
    {kIOMediumEthernetAuto,												0},
    {kIOMediumEthernet10BaseT 	 | kIOMediumOptionHalfDuplex,								10},
    {kIOMediumEthernet10BaseT 	 | kIOMediumOptionFullDuplex,								10},
    {kIOMediumEthernet100BaseTX  | kIOMediumOptionHalfDuplex,								100},
    {kIOMediumEthernet100BaseTX  | kIOMediumOptionFullDuplex,								100}
};

#define super IOEthernetController

OSDefineMetaClassAndStructors(AppleUSBCDCEEM, IOEthernetController);

#if USE_ELG
/****************************************************************************************************/
//
//		Function:	findKernelLoggerEED
//
//		Inputs:		
//
//		Outputs:	
//
//		Desc:		Just like the name says
//
/****************************************************************************************************/

IOReturn findKernelLoggerEED()
{
    OSIterator		*iterator = NULL;
    OSDictionary	*matchingDictionary = NULL;
    IOReturn		error = 0;
	
	// Get matching dictionary
	
    matchingDictionary = IOService::serviceMatching("com_apple_iokit_XTrace");
    if (!matchingDictionary)
    {
        error = kIOReturnError;
        IOLog(DEBUG_NAME "[findKernelLoggerEED] Couldn't create a matching dictionary.\n");
        goto exit;
    }
	
	// Get an iterator
	
    iterator = IOService::getMatchingServices(matchingDictionary);
    if (!iterator)
    {
        error = kIOReturnError;
        IOLog(DEBUG_NAME "[findKernelLoggerEED] No XTrace logger found.\n");
        goto exit;
    }
	
	// User iterator to find each com_apple_iokit_XTrace instance. There should be only one, so we
	// won't iterate
	
    gXTrace = (com_apple_iokit_XTrace*)iterator->getNextObject();
    if (gXTrace)
    {
        IOLog(DEBUG_NAME "[findKernelLoggerEED] Found XTrace logger at %p.\n", gXTrace);
    }
	
exit:
	
    if (error != kIOReturnSuccess)
    {
        gXTrace = NULL;
        IOLog(DEBUG_NAME "[findKernelLoggerEED] Could not find a logger instance. Error = %X.\n", error);
    }
	
    if (matchingDictionary)
        matchingDictionary->release();
            
    if (iterator)
        iterator->release();
		
    return error;
    
}/* end findKernelLoggerEED */
#endif

/****************************************************************************************************/
//
//		Function:	findCDCDriverEED
//
//		Inputs:		myDevice - Address of the controlling device
//				dataAddr - my address
//				dataInterfaceNum - the data interface number
//
//		Outputs:	
//
//		Desc:		Finds the initiating CDC driver and confirm the interface number
//
/****************************************************************************************************/

IOReturn findCDCDriverEED(IOUSBDevice *myDevice, void *dataAddr, UInt8 dataInterfaceNum)
{
    AppleUSBCDCEEM	*me = (AppleUSBCDCEEM *)dataAddr;
    AppleUSBCDC		*CDCDriver = NULL;
    bool		driverOK = false;
    OSIterator		*iterator = NULL;
    OSDictionary	*matchingDictionary = NULL;
    
    XTRACE(me, 0, 0, "findCDCDriverEED");
        
        // Get matching dictionary
       	
    matchingDictionary = IOService::serviceMatching("AppleUSBCDC");
    if (!matchingDictionary)
    {
        XTRACE(me, 0, 0, "findCDCDriverEED - Couldn't create a matching dictionary");
        return kIOReturnError;
    }
    
	// Get an iterator
	
    iterator = IOService::getMatchingServices(matchingDictionary);
    if (!iterator)
    {
        XTRACE(me, 0, 0, "findCDCDriverEED - No AppleUSBCDC driver found!");
        matchingDictionary->release();
        return kIOReturnError;
    }

#if 0    
	// Use iterator to find driver (there's only one so we won't bother to iterate)
                
    CDCDriver = (AppleUSBCDC *)iterator->getNextObject();
    if (CDCDriver)
    {
        driverOK = CDCDriver->confirmDriver(kUSBEthernetControlModel, dataInterfaceNum);
    }
#endif

 	// Iterate until we find our matching CDC driver
                
    CDCDriver = (AppleUSBCDC *)iterator->getNextObject();
    while (CDCDriver)
    {
        XTRACE(me, 0, CDCDriver, "findCDCDriverEED - CDC driver candidate");
        
        if (me->fDataInterface->GetDevice() == CDCDriver->getCDCDevice())
        {
            XTRACE(me, 0, CDCDriver, "findCDCDriverEED - Found our CDC driver");
            driverOK = CDCDriver->confirmDriver(kUSBEthernetControlModel, dataInterfaceNum);
            break;
        }
        CDCDriver = (AppleUSBCDC *)iterator->getNextObject();
    }

    matchingDictionary->release();
    iterator->release();
    
    if (!CDCDriver)
    {
        XTRACE(me, 0, 0, "findCDCDriverEED - CDC driver not found");
        return kIOReturnError;
    }
   
    if (!driverOK)
    {
        XTRACE(me, kUSBEthernetControlModel, dataInterfaceNum, "findCDCDriverEED - Not my interface");
        return kIOReturnError;
    }
    
    me->fConfigAttributes = CDCDriver->fbmAttributes;
    
    return kIOReturnSuccess;
    
}/* end findCDCDriverEED */

#if LOG_DATA
#define dumplen		32		// Set this to the number of bytes to dump and the rest should work out correct

#define buflen		((dumplen*2)+dumplen)+3
#define Asciistart	(dumplen*2)+3

/****************************************************************************************************/
//
//		Function:	AppleUSBCDCEEM::USBLogData
//
//		Inputs:		Dir - direction
//				Count - number of bytes
//				buf - the data
//
//		Outputs:	
//
//		Desc:		Puts the data in the log. 
//
/****************************************************************************************************/

void AppleUSBCDCEEM::USBLogData(UInt8 Dir, UInt32 Count, char *buf)
{    
    SInt32	wlen;
    UInt8	tDir = Dir;
#if USE_ELG
    UInt8 	*b;
    UInt8 	w[8];
#else
    UInt32	llen, rlen;
    UInt16	i, Aspnt, Hxpnt;
    UInt8	wchr;
    char	LocBuf[buflen+1];
#endif
    
    switch (tDir)
    {
        case kDataIn:
#if USE_ELG
            XTRACE2(this, buf, Count, "USBLogData - Read Complete, address, size");
#else
            IOLog("AppleUSBCDCEEM: USBLogData - Read Complete, address = %8x, size = %8d\n", (UInt)buf, (UInt)Count);
#endif
            break;
        case kDataOut:
#if USE_ELG
            XTRACE2(this, buf, Count, "USBLogData - Write, address, size");
#else
            IOLog("AppleUSBCDCEEM: USBLogData - Write, address = %8x, size = %8d\n", (UInt)buf, (UInt)Count);
#endif
            break;
        case kDataOther:
#if USE_ELG
            XTRACE2(this, buf, Count, "USBLogData - Other, address, size");
#else
            IOLog("AppleUSBCDCEEM: USBLogData - Other, address = %8x, size = %8d\n", (UInt)buf, (UInt)Count);
#endif
            break;
        case kDataNone:
            tDir = kDataOther;
            break;
    }

#if DUMPALL
    wlen = Count;
#else
    if (Count > dumplen)
    {
        wlen = dumplen;
    } else {
        wlen = Count;
    }
#endif

    if (wlen == 0)
    {
#if USE_ELG
        XTRACE2(this, 0, Count, "USBLogData - No data, Count=0");
#else
        IOLog("AppleUSBCDCEEM: USBLogData - No data, Count=0\n");
#endif
        return;
    }

#if (USE_ELG)
    b = (UInt8 *)buf;
    while (wlen > 0)							// loop over the buffer
    {
        bzero(w, sizeof(w));						// zero it
        bcopy(b, w, min(wlen, 8));					// copy bytes over
    
        switch (tDir)
        {
            case kDataIn:
                XTRACE2(this, (w[0] << 24 | w[1] << 16 | w[2] << 8 | w[3]), (w[4] << 24 | w[5] << 16 | w[6] << 8 | w[7]), "USBLogData - Rx buffer dump");
                break;
            case kDataOut:
                XTRACE2(this, (w[0] << 24 | w[1] << 16 | w[2] << 8 | w[3]), (w[4] << 24 | w[5] << 16 | w[6] << 8 | w[7]), "USBLogData - Tx buffer dump");
                break;
            case kDataOther:
                XTRACE2(this, (w[0] << 24 | w[1] << 16 | w[2] << 8 | w[3]), (w[4] << 24 | w[5] << 16 | w[6] << 8 | w[7]), "USBLogData - Misc buffer dump");
                break;
        }
        wlen -= 8;							// adjust by 8 bytes for next time (if have more)
        b += 8;
    }
#else
    rlen = 0;
    do
    {
        for (i=0; i<=buflen; i++)
        {
            LocBuf[i] = 0x20;
        }
        LocBuf[i] = 0x00;
        
        if (wlen > dumplen)
        {
            llen = dumplen;
            wlen -= dumplen;
        } else {
            llen = wlen;
            wlen = 0;
        }
        Aspnt = Asciistart;
        Hxpnt = 0;
        for (i=1; i<=llen; i++)
        {
            wchr = buf[i-1];
            LocBuf[Hxpnt++] = Asciify(wchr >> 4);
            LocBuf[Hxpnt++] = Asciify(wchr);
            if ((wchr < 0x20) || (wchr > 0x7F)) 		// Non printable characters
            {
                LocBuf[Aspnt++] = 0x2E;				// Replace with a period
            } else {
                LocBuf[Aspnt++] = wchr;
            }
        }
        LocBuf[(llen + Asciistart) + 1] = 0x00;
        IOLog(LocBuf);
        IOLog("\n");
        IOSleep(Sleep_Time);					// Try and keep the log from overflowing
       
        rlen += llen;
        buf = &buf[rlen];
    } while (wlen != 0);
#endif 

}/* end USBLogData */

/****************************************************************************************************/
//
//		Function:	AppleUSBCDCEEM::dumpData
//
//		Inputs:		buf - the data
//				size - number of bytes
//
//		Outputs:	None
//
//		Desc:		Creates formatted data for the log (cannot be used at interrupt time) 
//
/****************************************************************************************************/

void AppleUSBCDCEEM::dumpData(char *buf, UInt32 size)
{
    SInt32	curr, len, dlen;

    IOLog("AppleUSBCDCEEM: dumpData - Address = %8x, size = %8d\n", (UInt)buf, (UInt)size);

    dlen = 0;
    len = size;
    
    for (curr=0; curr<size; curr+=dumplen)
    {
        if (len > dumplen)
        {
            dlen = dumplen;
        } else {
            dlen = len;
        }
        IOLog("%8x ", (UInt)&buf[curr]);
        USBLogData(kDataNone, dlen, &buf[curr]);
        len -= dlen;
    }
   
}/* end dumpData */
#endif

/****************************************************************************************************/
//
//		Method:		AppleUSBCDCEEM::dataReadComplete
//
//		Inputs:		obj - me
//				param - pool index
//				rc - return code
//				remaining - what's left
//
//		Outputs:	
//
//		Desc:		BulkIn pipe (Data interface) read completion routine
//
/****************************************************************************************************/

void AppleUSBCDCEEM::dataReadComplete(void *obj, void *param, IOReturn rc, UInt32 remaining)
{
    AppleUSBCDCEEM	*me = (AppleUSBCDCEEM*)obj;
    IOReturn		ior;
    UInt32		poolIndx = (UInt32)param;
    
    XTRACE(me, 0, poolIndx, "dataReadComplete");

    if (rc == kIOReturnSuccess)	// If operation returned ok
    {	
        XTRACE(me, 0, gControlDriver->fMax_Block_Size - remaining, "dataReadComplete - data length");
		
        meLogData(kDataIn, (gControlDriver->fMax_Block_Size - remaining), me->fPipeInBuff[poolIndx].pipeInBuffer);
	
            // Move the incoming bytes up the stack

//        me->receivePacket(me->fPipeInBuff[poolIndx].pipeInBuffer, gControlDriver->fMax_Block_Size - remaining);
		me->receivePacket(me->fPipeInBuff[poolIndx].pipeInBuffer, me->fMax_Block_Size - remaining);
	
    } else {
        XTRACE(me, 0, rc, "dataReadComplete - Read completion io err");
        if (rc != kIOReturnAborted)
        {
            rc = me->clearPipeStall(me->fInPipe);
            if (rc != kIOReturnSuccess)
            {
                XTRACE(me, 0, rc, "dataReadComplete - clear stall failed (trying to continue)");
            }
        }
    }
    
        // Queue the next read, only if not aborted
	
    if (rc != kIOReturnAborted)
    {
        ior = me->fInPipe->Read(me->fPipeInBuff[poolIndx].pipeInMDP, &me->fPipeInBuff[poolIndx].readCompletionInfo, NULL);
        if (ior != kIOReturnSuccess)
        {
            XTRACE(me, 0, ior, "dataReadComplete - Failed to queue read");
            me->fPipeInBuff[poolIndx].dead = true;
        }
    } else {
        XTRACE(me, poolIndx, 0, "dataReadComplete - Read terminated");
        me->fPipeInBuff[poolIndx].dead = true;
    }

    return;
	
}/* end dataReadComplete */

/****************************************************************************************************/
//
//		Method:		AppleUSBCDCEEM::dataWriteComplete
//
//		Inputs:		obj - me
//				param - pool index
//				rc - return code
//				remaining - what's left
//
//		Outputs:	
//
//		Desc:		BulkOut pipe (Data interface) write completion routine
//
/****************************************************************************************************/

void AppleUSBCDCEEM::dataWriteComplete(void *obj, void *param, IOReturn rc, UInt32 remaining)
{
    AppleUSBCDCEEM	*me = (AppleUSBCDCEEM *)obj;
    struct mbuf		*m;
    UInt32		pktLen = 0;
    UInt32		numbufs = 0;
    UInt32		poolIndx;

    poolIndx = (UInt32)param;
    if (me->fBufferPoolLock)
    {
        IOLockLock(me->fBufferPoolLock);
    }
    
    if (rc == kIOReturnSuccess)						// If operation returned ok
    {	
        XTRACE(me, rc, poolIndx, "dataWriteComplete");
        
        if (me->fPipeOutBuff[poolIndx].m != NULL)			// Null means zero length write
        {
            m = me->fPipeOutBuff[poolIndx].m;
            while (m)
            {
                pktLen += m->m_len;
                numbufs++;
                m = m->m_next;
            }
            
            me->freePacket(me->fPipeOutBuff[poolIndx].m);		// Free the mbuf
            me->fPipeOutBuff[poolIndx].m = NULL;
        
            if ((pktLen % me->fOutPacketSize) == 0)			// If it was a multiple of max packet size then we need to do a zero length write
            {
                XTRACE(me, rc, pktLen, "dataWriteComplete - writing zero length packet");
                me->fPipeOutBuff[poolIndx].pipeOutMDP->setLength(0);
                me->fPipeOutBuff[poolIndx].writeCompletionInfo.parameter = (void *)poolIndx;
                me->fOutPipe->Write(me->fPipeOutBuff[poolIndx].pipeOutMDP, &me->fPipeOutBuff[poolIndx].writeCompletionInfo);
            } else {
                me->fPipeOutBuff[poolIndx].avail = true;
            }
        } else {
            me->fPipeOutBuff[poolIndx].avail = true;			// Make the buffer available again
        }
    } else {
        XTRACE(me, rc, poolIndx, "dataWriteComplete - IO err");

        if (me->fPipeOutBuff[poolIndx].m != NULL)
        {
            me->freePacket(me->fPipeOutBuff[poolIndx].m);		// Free the mbuf anyway
            me->fPipeOutBuff[poolIndx].m = NULL;
            me->fPipeOutBuff[poolIndx].avail = true;
        }
        if (rc != kIOReturnAborted)
        {
            rc = me->clearPipeStall(me->fOutPipe);
            if (rc != kIOReturnSuccess)
            {
                XTRACE(me, 0, rc, "dataWriteComplete - clear stall failed (trying to continue)");
            }
        }
    }
    
    if (me->fBufferPoolLock)
    {
        IOLockUnlock(me->fBufferPoolLock);
    }
        
    return;
	
}/* end dataWriteComplete */

/****************************************************************************************************/
//
//		Method:		AppleUSBCDCEEM::probe
//
//		Inputs:		provider - my provider
//
//		Outputs:	IOService - from super::probe, score - probe score
//
//		Desc:		Modify the probe score if necessary (we don't  at the moment)
//
/****************************************************************************************************/

IOService* AppleUSBCDCEEM::probe( IOService *provider, SInt32 *score )
{ 
    IOService   *res;
	
		// If our IOUSBInterface has a "do not match" property, it means that we should not match and need 
		// to bail.  See rdar://3716623
    
    OSBoolean *boolObj = OSDynamicCast(OSBoolean, provider->getProperty("kDoNotClassMatchThisInterface"));
    if (boolObj && boolObj->isTrue())
    {
        XTRACE(this, 0, 0, "probe - provider doesn't want us to match");
        return NULL;
    }

    res = super::probe(provider, score);
    
    return res;
    
}/* end probe */

/****************************************************************************************************/
//
//		Method:		AppleUSBCDCEEM::init
//
//		Inputs:		properties - data (keys and values) used to match
//
//		Outputs:	Return code - true (init successful), false (init failed)
//
//		Desc:		Initialize the driver.
//
/****************************************************************************************************/

bool AppleUSBCDCEEM::init(OSDictionary *properties)
{
    UInt32	i;
        
#if USE_ELG
    XTraceLogInfo	*logInfo;
    
    findKernelLoggerEED();
    if (gXTrace)
    {
        gXTrace->retain();		// don't let it unload ...
        XTRACE(this, 0, 0xbeefbeef, "Hello from start");
        logInfo = gXTrace->LogGetInfo();
        IOLog("AppleUSBCDCEEM: init - Log is at %x\n", (unsigned int)logInfo);
    } else {
        return false;
    }
#endif

    XTRACE(this, 0, 0, "init");
    
    if (super::init(properties) == false)
    {
        XTRACE(this, 0, 0, "init - initialize super failed");
        return false;
    }
    
    for (i=0; i<kMaxOutBufPool; i++)
    {
        fPipeOutBuff[i].pipeOutMDP = NULL;
        fPipeOutBuff[i].pipeOutBuffer = NULL;
        fPipeOutBuff[i].m = NULL;
        fPipeOutBuff[i].avail = false;
        fPipeOutBuff[i].writeCompletionInfo.target = NULL;
        fPipeOutBuff[i].writeCompletionInfo.action = NULL;
        fPipeOutBuff[i].writeCompletionInfo.parameter = NULL;
    }
    fOutPoolIndex = 0;
    
    for (i=0; i<kMaxInBufPool; i++)
    {
        fPipeInBuff[i].pipeInMDP = NULL;
        fPipeInBuff[i].pipeInBuffer = NULL;
        fPipeInBuff[i].dead = false;
        fPipeInBuff[i].readCompletionInfo.target = NULL;
        fPipeInBuff[i].readCompletionInfo.action = NULL;
        fPipeInBuff[i].readCompletionInfo.parameter = NULL;
    }

    return true;

}/* end init*/

/****************************************************************************************************/
//
//		Method:		AppleUSBCDCEEM::start
//
//		Inputs:		provider - my provider
//
//		Outputs:	Return code - true (it's me), false (sorry it probably was me, but I can't configure it)
//
//		Desc:		This is called once it has beed determined I'm probably the best 
//				driver for this device.
//
/****************************************************************************************************/

bool AppleUSBCDCEEM::start(IOService *provider)
{
    OSNumber		*bufNumber = NULL;
    UInt16		bufValue = 0;

    XTRACE(this, 0, provider, "start");
    
    if(!super::start(provider))
    {
        ALERT(0, 0, "start - start super failed");
        return false;
    }

	// Get my USB provider - the interface

    fDataInterface = OSDynamicCast(IOUSBInterface, provider);
    if(!fDataInterface)
    {
        ALERT(0, 0, "start - provider invalid");
        return false;
    }
    
    fDataInterfaceNumber = fDataInterface->GetInterfaceNumber();
    
    if (findCDCDriverEED(fDataInterface->GetDevice(), this, fDataInterfaceNumber) != kIOReturnSuccess)
    {
        XTRACE(this, 0, 0, "start - Find CDC driver failed");
		super::stop(provider);
        return false;
    }
    
    fBufferPoolLock = IOLockAlloc();
    if (!fBufferPoolLock)
    {
        ALERT(0, 0, "start - Buffer pool lock allocate failed");
        return false;
    }
    
        // get workloop
        
    fWorkLoop = getWorkLoop();
    if (!fWorkLoop)
    {
        ALERT(0, 0, "start - getWorkLoop failed");
        return false;
    }
    
    if (!configureData())
    {
        ALERT(0, 0, "start - configureData failed");
        return false;
    }
    
		// Check for an input buffer pool override first
	
	fInBufPool = 0;
	fOutBufPool = 0;
		
	bufNumber = (OSNumber *)provider->getProperty(inputTag);
    if (bufNumber)
    {
		bufValue = bufNumber->unsigned16BitValue();
		XTRACE(this, 0, bufValue, "start - Number of input buffers override value");
        if (bufValue <= kMaxInBufPool)
        {
            fInBufPool = bufValue;
        } else {
            fInBufPool = kMaxInBufPool;
        }
	} else {
		fInBufPool = 0;
	}
    
		// Now set up the real input buffer pool values (only if not overridden)
    
	if (fInBufPool == 0)
	{
		bufNumber = NULL;
		bufNumber = (OSNumber *)getProperty(inputTag);
		if (bufNumber)
		{
			bufValue = bufNumber->unsigned16BitValue();
			XTRACE(this, 0, bufValue, "start - Number of input buffers requested");
			if (bufValue <= kMaxInBufPool)
			{
				fInBufPool = bufValue;
			} else {
				fInBufPool = kMaxInBufPool;
			}
		} else {
			fInBufPool = kInBufPool;
		}
    }
	
		// Check for an output buffer pool override
		
	bufNumber = NULL;
	bufNumber = (OSNumber *)provider->getProperty(outputTag);
    if (bufNumber)
    {
		bufValue = bufNumber->unsigned16BitValue();
		XTRACE(this, 0, bufValue, "start - Number of output buffers override value");
        if (bufValue <= kMaxInBufPool)
        {
            fOutBufPool = bufValue;
        } else {
            fOutBufPool = kMaxOutBufPool;
        }
	} else {
		fOutBufPool = 0;
	}
    
        // Now set up the real output buffer pool values (only if not overridden)
    
	if (fOutBufPool == 0)
	{
		bufNumber = NULL;
		bufNumber = (OSNumber *)getProperty(outputTag);
		if (bufNumber)
		{
			bufValue = bufNumber->unsigned16BitValue();
			XTRACE(this, 0, bufValue, "start - Number of output buffers requested");
			if (bufValue <= kMaxOutBufPool)
			{
				fOutBufPool = bufValue;
			} else {
				fOutBufPool = kMaxOutBufPool;
			}
		} else {
			fOutBufPool = kOutBufPool;
		}
	}
    
    XTRACE(this, fInBufPool, fOutBufPool, "start - Buffer pools (input, output)");
    
    if (!createNetworkInterface())
    {
        ALERT(0, 0, "start - createNetworkInterface failed");
        return false;
    }
	
         // Looks like we're ok
    
    fDataInterface->retain();
    fWorkLoop->retain();
    fTransmitQueue->retain();
    
        // Ready to service interface requests
    
    fNetworkInterface->registerService();
        
    XTRACE(this, 0, 0, "start - successful");
	IOLog(DEBUG_NAME ": Version number - %s, Input buffers %d, Output buffers %d\n", VersionNumber, fInBufPool, fOutBufPool);
    
    return true;
    	
}/* end start */

/****************************************************************************************************/
//
//		Method:		AppleUSBCDCEEM::stop
//
//		Inputs:		provider - my provider
//
//		Outputs:	
//
//		Desc:		Stops the driver
//
/****************************************************************************************************/

void AppleUSBCDCEEM::stop(IOService *provider)
{
    
    XTRACE(this, 0, 0, "stop");
    
        // Release all resources
		
    releaseResources();
    
    if (fDataInterface)	
    { 
        fDataInterface->close(this);
        fDataInterface->release();
        fDataInterface = NULL;
    }
    
    if (fNetworkInterface)
    {
        fNetworkInterface->release();
        fNetworkInterface = NULL;
    }

    if (fMediumDict)
    {
        fMediumDict->release();
        fMediumDict = NULL;
    }
    
    if (fBufferPoolLock)
    {
        IOLockFree(fBufferPoolLock);
        fBufferPoolLock = NULL;
    }
    
    if (fWorkLoop)
    {
        fWorkLoop->release();
        fWorkLoop = NULL;
    }
    
    if (fTransmitQueue)
    {
        fTransmitQueue->release();
        fTransmitQueue = NULL;
    }
    
    super::stop(provider);
    
    return;
	
}/* end stop */

/****************************************************************************************************/
//
//		Method:		AppleUSBCDCEEM::configureData
//
//		Inputs:		
//
//		Outputs:	return code - true (configure was successful), false (it failed)
//
//		Desc:		Finishes up the rest of the configuration
//
/****************************************************************************************************/

bool AppleUSBCDCEEM::configureData()
{
    IOUSBFindInterfaceRequest		req;
    const IOUSBInterfaceDescriptor	*altInterfaceDesc;
    IOReturn				ior = kIOReturnSuccess;
    UInt16				numends = 0;
    UInt16				alt;

    XTRACE(this, 0, 0, "configureData.");
    
    if (!fDataInterface)
    {
        XTRACE(this, 0, 0, "configureData - Data interface is NULL");
        return false;
    }

    if (!fDataInterface->open(this))
    {
        XTRACE(this, 0, 0, "configureData - open data interface failed");
        fDataInterface->release();
        fDataInterface = NULL;
        return false;
    }
        
        // Check we have the correct interface (there maybe an alternate)
    
    numends = fDataInterface->GetNumEndpoints();
    if (numends < 2)
    {
        req.bInterfaceClass = kUSBDataClass;
        req.bInterfaceSubClass = 0;
        req.bInterfaceProtocol = 0;
        req.bAlternateSetting = kIOUSBFindInterfaceDontCare;
        altInterfaceDesc = fDataInterface->FindNextAltInterface(NULL, &req);
        if (!altInterfaceDesc)
        {
            XTRACE(this, 0, 0, "configureData - FindNextAltInterface failed");
            return false;
        }
        while (altInterfaceDesc)
        {
            numends = altInterfaceDesc->bNumEndpoints;
            if (numends > 1)
            {
                alt = altInterfaceDesc->bAlternateSetting;
                XTRACE(this, numends, alt, "configureData - Data Class interface (alternate) found");
                ior = fDataInterface->SetAlternateInterface(this, alt);
                if (ior == kIOReturnSuccess)
                {
                    XTRACE(this, 0, 0, "configureData - Alternate set");
                    break;
                } else {
                    XTRACE(this, 0, 0, "configureData - SetAlternateInterface failed");
                    return false;
                }
            } else {
                XTRACE(this, 0, altInterfaceDesc, "configureData - No endpoints this alternate");
            }
            altInterfaceDesc = fDataInterface->FindNextAltInterface(altInterfaceDesc, &req);
        }
    }
    
    if (numends < 2)
    {
        XTRACE(this, 0, 0, "configureData - Could not find the correct interface");
        return false;
    }
		
    return true;
	
}/* end configureData */

/****************************************************************************************************/
//
//		Method:		AppleUSBCDCEEM::createNetworkInterface
//
//		Inputs:		
//
//		Outputs:	return Code - true (created and initialilzed ok), false (it failed)
//
//		Desc:		Creates and initializes the network interface
//
/****************************************************************************************************/

bool AppleUSBCDCEEM::createNetworkInterface()
{
	
    XTRACE(this, 0, 0, "createNetworkInterface");
    
            // Allocate memory for transmit queue

    fTransmitQueue = (IOGatedOutputQueue *)getOutputQueue();
    if (!fTransmitQueue) 
    {
        ALERT(0, 0, "createNetworkInterface - Output queue initialization failed");
        return false;
    }
    
        // Attach an IOEthernetInterface client
        
    XTRACE(this, 0, 0, "createNetworkInterface - attaching and registering interface");
    
    if (!attachInterface((IONetworkInterface **)&fNetworkInterface, false))
    {	
        ALERT(0, 0, "createNetworkInterface - attachInterface failed");      
        return false;
    }
    
    XTRACE(this, 0, 0, "createNetworkInterface - Exiting, successful");

    return true;
	
}/* end createNetworkInterface */

/****************************************************************************************************/
//
//		Method:		AppleUSBCDCEEM::enable
//
//		Inputs:		netif - the interface being enabled
//
//		Outputs:	Return code - kIOReturnSuccess or kIOReturnIOError
//
//		Desc:		Called by IOEthernetInterface client to enable the controller.
//				This method is always called while running on the default workloop
//				thread
//
/****************************************************************************************************/

IOReturn AppleUSBCDCEEM::enable(IONetworkInterface *netif)
{
    IONetworkMedium	*medium;
    IOMediumType    	mediumType = kIOMediumEthernet10BaseT | kIOMediumOptionFullDuplex;
    
    XTRACE(this, 0, netif, "enable");
    
    IOSleep(5);				// Just in case (to let start finish - on another thread)

        // If an interface client has previously enabled us,
        // and we know there can only be one interface client
        // for this driver, then simply return success.

    if (fNetifEnabled)
    {
        XTRACE(this, 0, 0, "enable - already enabled");
        return kIOReturnSuccess;
    }
        
    if (!fReady)
    {
        if (!wakeUp())
        {
            XTRACE(this, 0, fReady, "enable - wakeUp failed");
            return kIOReturnIOError;
        }
    }
    
        // Mark the controller as enabled by the interface.

    fNetifEnabled = true;
    
        // Assume an active link (leave this in for now - until we know better)
        // Should probably use the values returned in the Network Connection notification
        // that is if we have an interrupt pipe, otherwise default to these
    
    fLinkStatus = 1;
    
    medium = IONetworkMedium::getMediumWithType(fMediumDict, mediumType);
    XTRACE(this, mediumType, medium, "enable - medium type and pointer");
    setLinkStatus(kIONetworkLinkActive | kIONetworkLinkValid, medium, 10 * 1000000);
    XTRACE(this, 0, 0, "enable - LinkStatus set");
    
        // Start our IOOutputQueue object.

    fTransmitQueue->setCapacity(TRANSMIT_QUEUE_SIZE);
    XTRACE(this, 0, TRANSMIT_QUEUE_SIZE, "enable - capicity set");
    fTransmitQueue->start();
    XTRACE(this, 0, 0, "enable - transmit queue started");
    
    return kIOReturnSuccess;
    
}/* end enable */

/****************************************************************************************************/
//
//		Method:		AppleUSBCDCEEM::disable
//
//		Inputs:		netif - the interface being disabled
//
//		Outputs:	Return code - kIOReturnSuccess
//
//		Desc:		Called by IOEthernetInterface client to disable the controller.
//				This method is always called while running on the default workloop
//				thread
//
/****************************************************************************************************/
 
IOReturn AppleUSBCDCEEM::disable(IONetworkInterface *netif)
{

    XTRACE(this, 0, 0, "disable");

        // Disable our IOOutputQueue object. This will prevent the
        // outputPacket() method from being called
        
    fTransmitQueue->stop();

        // Flush all packets currently in the output queue

    fTransmitQueue->setCapacity(0);
    fTransmitQueue->flush();

    putToSleep();

    fNetifEnabled = false;
    fReady = false;
    
    return kIOReturnSuccess;
    
}/* end disable */

/****************************************************************************************************/
//
//		Method:		AppleUSBCDCEEM::setWakeOnMagicPacket
//
//		Inputs:		active - true(wake), false(don't)
//
//		Outputs:	Return code - kIOReturnSuccess
//
//		Desc:		Set for wake on magic packet
//
/****************************************************************************************************/

IOReturn AppleUSBCDCEEM::setWakeOnMagicPacket(bool active)
{
    IOUSBDevRequest	devreq;
    IOReturn		ior = kIOReturnSuccess;

    XTRACE(this, 0, active, "setWakeOnMagicPacket");
	
    fWOL = active;
    
    if (fConfigAttributes & kUSBAtrRemoteWakeup)
    {
    
            // Clear the feature if wake-on-lan is not set (SetConfiguration sets the feature 
            // automatically if the device supports remote wake up)
    
        if (!active)				
        {
            devreq.bmRequestType = USBmakebmRequestType(kUSBOut, kUSBStandard, kUSBDevice);
            devreq.bRequest = kUSBRqClearFeature;
            devreq.wValue = kUSBFeatureDeviceRemoteWakeup;
            devreq.wIndex = 0;
            devreq.wLength = 0;
            devreq.pData = 0;

            ior = fDataInterface->GetDevice()->DeviceRequest(&devreq);
            if (ior == kIOReturnSuccess)
            {
                XTRACE(this, 0, ior, "setWakeOnMagicPacket - Clearing remote wake up feature successful");
            } else {
                XTRACE(this, 0, ior, "setWakeOnMagicPacket - Clearing remote wake up feature failed");
            }
        }
    } else {
        XTRACE(this, 0, 0, "setWakeOnMagicPacket - Remote wake up not supported");
    }

    
    return kIOReturnSuccess;
    
}/* end setWakeOnMagicPacket */

/****************************************************************************************************/
//
//		Method:		AppleUSBCDCEEM::getPacketFilters
//
//		Inputs:		group - the filter group
//
//		Outputs:	Return code - kIOReturnSuccess and others
//				filters - the capability
//
//		Desc:		Set the filter capability for the driver
//
/****************************************************************************************************/

IOReturn AppleUSBCDCEEM::getPacketFilters(const OSSymbol *group, UInt32 *filters) const
{
    IOReturn	rtn = kIOReturnSuccess;
    
    XTRACE(this, group, filters, "getPacketFilters");

    if (group == gIOEthernetWakeOnLANFilterGroup)
    {
        if (fConfigAttributes & kUSBAtrRemoteWakeup)
        {
            *filters = kIOEthernetWakeOnMagicPacket;
        } else {
            *filters = 0;
        }
    } else {
        if (group == gIONetworkFilterGroup)
        {
            *filters = kIOPacketFilterUnicast | kIOPacketFilterBroadcast;
        } else {
            rtn = super::getPacketFilters(group, filters);
        }
    }
    
    if (rtn != kIOReturnSuccess)
    {
        XTRACE(this, 0, rtn, "getPacketFilters - failed");
    }
    
    return rtn;
    
}/* end getPacketFilters */

/****************************************************************************************************/
//
//		Method:		AppleUSBCDCEEM::selectMedium
//
//		Inputs:
//
//		Outputs:
//
//		Desc:		Lets us know if someone is playing with ifconfig
//
/****************************************************************************************************/

IOReturn AppleUSBCDCEEM::selectMedium(const IONetworkMedium *medium)
{
    
    XTRACE(this, 0, 0, "selectMedium");

    setSelectedMedium(medium);
    
    return kIOReturnSuccess;
        
}/* end selectMedium */

/****************************************************************************************************/
//
//		Method:		AppleUSBCDCEEM::getHardwareAddress
//
//		Inputs:		
//
//		Outputs:	Return code - kIOReturnSuccess or kIOReturnError
//				ea - the address
//
//		Desc:		Make up an ethernet address for now
//					***** THIS IS TEMPORARY AND MUST BE CHANGED *****
//
/****************************************************************************************************/

IOReturn AppleUSBCDCEEM::getHardwareAddress(IOEthernetAddress *ea)
{
    UInt32      i;
	OSNumber	*location;
    UInt32		locVal;
	UInt8		*rlocVal;

    XTRACE(this, 0, 0, "getHardwareAddress");
	
	location = (OSNumber *)fDataInterface->GetDevice()->getProperty(kUSBDevicePropertyLocationID);
	if (location)
	{
		locVal = location->unsigned32BitValue();
		rlocVal = (UInt8*)&locVal;
		ea->bytes[0] = 0x00;
		ea->bytes[1] = 0x03;
		for (i=0; i<4; i++)
		{
			ea->bytes[i+2] = rlocVal[i];
		}
	} else {
		XTRACE(this, 0, 0, "getHardwareAddress - Get location failed");
		return kIOReturnError;
	}

    return kIOReturnSuccess;
    
}/* end getHardwareAddress */

/****************************************************************************************************/
//
//		Method:		AppleUSBCDCEEM::newVendorString
//
//		Inputs:		
//
//		Outputs:	Return code - the vendor string
//
//		Desc:		Identifies the hardware vendor
//
/****************************************************************************************************/

const OSString* AppleUSBCDCEEM::newVendorString() const
{

    XTRACE(this, 0, 0, "newVendorString");
    
    return OSString::withCString((const char *)defaultName);		// Maybe we should use the descriptors

}/* end newVendorString */

/****************************************************************************************************/
//
//		Method:		AppleUSBCDCEEM::newModelString
//
//		Inputs:		
//
//		Outputs:	Return code - the model string
//
//		Desc:		Identifies the hardware model
//
/****************************************************************************************************/

const OSString* AppleUSBCDCEEM::newModelString() const
{

    XTRACE(this, 0, 0, "newModelString");
    
    return OSString::withCString("USB");		// Maybe we should use the descriptors
    
}/* end newModelString */

/****************************************************************************************************/
//
//		Method:		AppleUSBCDCEEM::newRevisionString
//
//		Inputs:		
//
//		Outputs:	Return code - the revision string
//
//		Desc:		Identifies the hardware revision
//
/****************************************************************************************************/

const OSString* AppleUSBCDCEEM::newRevisionString() const
{

    XTRACE(this, 0, 0, "newRevisionString");
    
    return OSString::withCString("");
    
}/* end newRevisionString */

/****************************************************************************************************/
//
//		Method:		AppleUSBCDCEEM::setMulticastMode
//
//		Inputs:		active - true (set it), false (don't)
//
//		Outputs:	Return code - kIOReturnSuccess
//
//		Desc:		Sets multicast mode (not supported in this driver)
//
/****************************************************************************************************/

IOReturn AppleUSBCDCEEM::setMulticastMode(bool active)
{

    XTRACE(this, 0, active, "setMulticastMode");
    
    return kIOReturnIOError;
    
}/* end setMulticastMode */

/****************************************************************************************************/
//
//		Method:		AppleUSBCDCEEM::setMulticastList
//
//		Inputs:		addrs - list of addresses
//				count - number in the list
//
//		Outputs:	Return code - kIOReturnSuccess or kIOReturnIOError
//
//		Desc:		Sets multicast list (not supported in this driver
//
/****************************************************************************************************/

IOReturn AppleUSBCDCEEM::setMulticastList(IOEthernetAddress *addrs, UInt32 count)
{
//    bool	uStat;
    
    XTRACE(this, addrs, count, "setMulticastList");
    
    return kIOReturnIOError;
    
}/* end setMulticastList */

/****************************************************************************************************/
//
//		Method:		AppleUSBCDCEEM::setPromiscuousMode
//
//		Inputs:		active - true (set it), false (don't)
//
//		Outputs:	Return code - kIOReturnSuccess
//
//		Desc:		Sets promiscuous mode (not supported by this driver)
//
/****************************************************************************************************/

IOReturn AppleUSBCDCEEM::setPromiscuousMode(bool active)
{
    
    XTRACE(this, 0, active, "setPromiscuousMode");
    
    return kIOReturnIOError;
    
}/* end setPromiscuousMode */

/****************************************************************************************************/
//
//		Method:		AppleUSBCDCEEM::createOutputQueue
//
//		Inputs:		
//
//		Outputs:	Return code - the output queue
//
//		Desc:		Creates the output queue
//
/****************************************************************************************************/

IOOutputQueue* AppleUSBCDCEEM::createOutputQueue()
{

    XTRACE(this, 0, 0, "createOutputQueue");
    
    return IOBasicOutputQueue::withTarget(this, TRANSMIT_QUEUE_SIZE);
    
}/* end createOutputQueue */

/****************************************************************************************************/
//
//		Method:		AppleUSBCDCEEM::outputPacket
//
//		Inputs:		mbuf - the packet
//				param - optional parameter
//
//		Outputs:	Return code - kIOReturnOutputSuccess or kIOReturnOutputStall
//
//		Desc:		Packet transmission. The BSD mbuf needs to be formatted correctly
//				and transmitted
//
/****************************************************************************************************/

UInt32 AppleUSBCDCEEM::outputPacket(struct mbuf *pkt, void *param)
{
    UInt32	ior = kIOReturnSuccess;
    
    XTRACE(this, pkt, 0, "outputPacket");

    if (!fLinkStatus)
    {
        XTRACE(this, pkt, fLinkStatus, "outputPacket - link is down");
		fpNetStats->outputErrors++;
        freePacket(pkt);
        return kIOReturnOutputDropped;
    }
    
    ior = USBTransmitPacket(pkt);
    if (ior != kIOReturnSuccess)
    {
        return kIOReturnOutputStall;
    }

    return kIOReturnOutputSuccess;
    
}/* end outputPacket */

/****************************************************************************************************/
//
//		Method:		AppleUSBCDCEEM::configureInterface
//
//		Inputs:		netif - the interface being configured
//
//		Outputs:	Return code - true (configured ok), false (not)
//
//		Desc:		Finish the network interface configuration
//
/****************************************************************************************************/

bool AppleUSBCDCEEM::configureInterface(IONetworkInterface *netif)
{
    IONetworkData	*nd;

    XTRACE(this, IOThreadSelf(), netif, "configureInterface");

    if (super::configureInterface(netif) == false)
    {
        ALERT(0, 0, "configureInterface - super failed");
        return false;
    }
    
        // Get a pointer to the statistics structure in the interface

    nd = netif->getNetworkData(kIONetworkStatsKey);
    if (!nd || !(fpNetStats = (IONetworkStats *)nd->getBuffer()))
    {
        ALERT(0, 0, "configureInterface - Invalid network statistics");
        return false;
    }

        // Get the Ethernet statistics structure

    nd = netif->getParameter(kIOEthernetStatsKey);
    if (!nd || !(fpEtherStats = (IOEthernetStats*)nd->getBuffer()))
    {
        ALERT(0, 0, "configureInterface - Invalid ethernet statistics\n");
        return false;
    }

    return true;
    
}/* end configureInterface */

/****************************************************************************************************/
//
//		Method:		AppleUSBCDCEEM::wakeUp
//
//		Inputs:		
//
//		Outputs:	Return Code - true(we're awake), false(failed)
//
//		Desc:		Resumes the device it it was suspended and then gets all the data
//				structures sorted out and all the pipes ready.
//
/****************************************************************************************************/

bool AppleUSBCDCEEM::wakeUp()
{
    IOReturn 	rtn = kIOReturnSuccess;
    UInt32	i;
    bool	readOK = false;

    XTRACE(this, 0, 0, "wakeUp");
    
    fReady = false;
    
    setLinkStatus(0, 0);				// Initialize the link state
    
    if (!allocateResources()) 
    {
        ALERT(0, 0, "wakeUp - allocateResources failed");
    	return false;
    }

        // Kick off the data-in bulk pipe reads
    
    for (i=0; i<fInBufPool; i++)
    {
        if (fPipeInBuff[i].pipeInMDP)
        {
            fPipeInBuff[i].readCompletionInfo.parameter = (void *)i;
            rtn = fInPipe->Read(fPipeInBuff[i].pipeInMDP, &fPipeInBuff[i].readCompletionInfo, NULL);
            if (rtn == kIOReturnSuccess)
            {
                readOK = true;
            } else {
                XTRACE(this, i, rtn, "wakeUp - Read failed");
            }
        }
    }
			
    if (!readOK)
    {
    
    	// We failed for some reason
	
        ALERT(0, 0, "wakeUp - Starting the input pipe read(s) failed");
        return false;
    } else {
        if (!fMediumDict)
        {
            if (!createMediumTables())
            {
                ALERT(0, 0, "wakeUp - createMediumTables failed");
                return false;
            }
        }

        fReady = true;
    }

    return true;
	
}/* end wakeUp */

/****************************************************************************************************/
//
//		Method:		AppleUSBCDCEEM::putToSleep
//
//		Inputs:		
//
//		Outputs:	Return Code - true(we're asleep), false(failed)
//
//		Desc:		Do clean up and suspend the device.
//
/****************************************************************************************************/

void AppleUSBCDCEEM::putToSleep()
{

    XTRACE(this, 0, 0, "putToSleep");
        
    fReady = false;

    setLinkStatus(0, 0);

}/* end putToSleep */

/****************************************************************************************************/
//
//		Method:		AppleUSBCDCEEM::createMediumTables
//
//		Inputs:		
//
//		Outputs:	Return code - true (tables created), false (not created)
//
//		Desc:		Creates the medium tables
//
/****************************************************************************************************/

bool AppleUSBCDCEEM::createMediumTables()
{
    IONetworkMedium	*medium;
    UInt64		maxSpeed;
    UInt32		i;

    XTRACE(this, 0, 0, "createMediumTables");

    maxSpeed = 100;
    fMediumDict = OSDictionary::withCapacity(sizeof(mediumTable) / sizeof(mediumTable[0]));
    if (fMediumDict == 0)
    {
        XTRACE(this, 0, 0, "createMediumTables - create dict. failed");
        return false;
    }

    for (i = 0; i < sizeof(mediumTable) / sizeof(mediumTable[0]); i++)
    {
        medium = IONetworkMedium::medium(mediumTable[i].type, mediumTable[i].speed);
        if (medium && (medium->getSpeed() <= maxSpeed))
        {
            IONetworkMedium::addMedium(fMediumDict, medium);
            medium->release();
        }
    }

    if (publishMediumDictionary(fMediumDict) != true)
    {
        XTRACE(this, 0, 0, "createMediumTables - publish dict. failed");
        return false;
    }

    medium = IONetworkMedium::getMediumWithType(fMediumDict, kIOMediumEthernetAuto);
    setCurrentMedium(medium);

    return true;
    
}/* end createMediumTables */

/****************************************************************************************************/
//
//		Method:		AppleUSBCDCEEM::allocateResources
//
//		Inputs:		
//
//		Outputs:	return code - true (allocate was successful), false (it failed)
//
//		Desc:		Gets all the endpoints open and buffers allocated etc.
//
/****************************************************************************************************/

bool AppleUSBCDCEEM::allocateResources()
{
    IOUSBFindEndpointRequest		epReq;
    UInt32				i;

    XTRACE(this, 0, 0, "allocateResources.");

        // Open all the end points

    epReq.type = kUSBBulk;
    epReq.direction = kUSBIn;
    epReq.maxPacketSize	= 0;
    epReq.interval = 0;
    fInPipe = fDataInterface->FindNextPipe(0, &epReq);
    if (!fInPipe)
    {
        XTRACE(this, 0, 0, "allocateResources - no bulk input pipe.");
        return false;
    }
    XTRACE(this, epReq.maxPacketSize << 16 |epReq.interval, fInPipe, "allocateResources - bulk input pipe.");

    epReq.direction = kUSBOut;
    fOutPipe = fDataInterface->FindNextPipe(0, &epReq);
    if (!fOutPipe)
    {
        XTRACE(this, 0, 0, "allocateResources - no bulk output pipe.");
        return false;
    }
    fOutPacketSize = epReq.maxPacketSize;
    XTRACE(this, epReq.maxPacketSize << 16 |epReq.interval, fOutPipe, "allocateResources - bulk output pipe.");
    
        // Allocate Memory Descriptor Pointer with memory for the data-in bulk pipe

    for (i=0; i<fInBufPool; i++)
    {
//        fPipeInBuff[i].pipeInMDP = IOBufferMemoryDescriptor::withCapacity(gControlDriver->fMax_Block_Size, kIODirectionIn);
		fPipeInBuff[i].pipeInMDP = IOBufferMemoryDescriptor::withCapacity(fMax_Block_Size, kIODirectionIn);
        if (!fPipeInBuff[i].pipeInMDP)
        {
            XTRACE(this, 0, i, "allocateResources - Allocate input descriptor failed");
            return false;
        }
		
//        fPipeInBuff[i].pipeInMDP->setLength(gControlDriver->fMax_Block_Size);
		fPipeInBuff[i].pipeInMDP->setLength(fMax_Block_Size);
        fPipeInBuff[i].pipeInBuffer = (UInt8*)fPipeInBuff[i].pipeInMDP->getBytesNoCopy();
        XTRACE(this, fPipeInBuff[i].pipeInMDP, fPipeInBuff[i].pipeInBuffer, "allocateResources - input buffer");
        fPipeInBuff[i].dead = false;
        fPipeInBuff[i].readCompletionInfo.target = this;
        fPipeInBuff[i].readCompletionInfo.action = dataReadComplete;
        fPipeInBuff[i].readCompletionInfo.parameter = NULL;
    }
    
        // Allocate Memory Descriptor Pointers with memory for the data-out bulk pipe pool

    for (i=0; i<fOutBufPool; i++)
    {
//        fPipeOutBuff[i].pipeOutMDP = IOBufferMemoryDescriptor::withCapacity(gControlDriver->fMax_Block_Size, kIODirectionOut);
		fPipeOutBuff[i].pipeOutMDP = IOBufferMemoryDescriptor::withCapacity(fMax_Block_Size, kIODirectionOut);
        if (!fPipeOutBuff[i].pipeOutMDP)
        {
            XTRACE(this, 0, i, "allocateResources - Allocate output descriptor failed");
            return false;
        }
		
//        fPipeOutBuff[i].pipeOutMDP->setLength(gControlDriver->fMax_Block_Size);
		fPipeOutBuff[i].pipeOutMDP->setLength(fMax_Block_Size);
        fPipeOutBuff[i].pipeOutBuffer = (UInt8*)fPipeOutBuff[i].pipeOutMDP->getBytesNoCopy();
        XTRACE(this, fPipeOutBuff[i].pipeOutMDP, fPipeOutBuff[i].pipeOutBuffer, "allocateResources - output buffer");
        fPipeOutBuff[i].avail = true;
        fPipeOutBuff[i].writeCompletionInfo.target = this;
        fPipeOutBuff[i].writeCompletionInfo.action = dataWriteComplete;
        fPipeOutBuff[i].writeCompletionInfo.parameter = NULL;				// for now, filled in with pool index when sent
    }
		
    return true;
	
}/* end allocateResources */

/****************************************************************************************************/
//
//		Method:		AppleUSBCDCEEM::releaseResources
//
//		Inputs:		
//
//		Outputs:	
//
//		Desc:		Frees up the resources allocated in allocateResources
//
/****************************************************************************************************/

void AppleUSBCDCEEM::releaseResources()
{
    UInt32	i;
    
    XTRACE(this, 0, 0, "releaseResources");

    for (i=0; i<fOutBufPool; i++)
    {
        if (fPipeOutBuff[i].pipeOutMDP)	
        { 
            fPipeOutBuff[i].pipeOutMDP->release();	
            fPipeOutBuff[i].pipeOutMDP = NULL;
            fPipeOutBuff[i].avail = false;
            fPipeOutBuff[i].writeCompletionInfo.target = NULL;
            fPipeOutBuff[i].writeCompletionInfo.action = NULL;
            fPipeOutBuff[i].writeCompletionInfo.parameter = NULL;
        }
    }
    fOutPoolIndex = 0;
    
    for (i=0; i<fInBufPool; i++)
    {
        if (fPipeInBuff[i].pipeInMDP)	
        { 
            fPipeInBuff[i].pipeInMDP->release();	
            fPipeInBuff[i].pipeInMDP = NULL;
            fPipeInBuff[i].dead = false;
            fPipeInBuff[i].readCompletionInfo.target = NULL;
            fPipeInBuff[i].readCompletionInfo.action = NULL;
            fPipeInBuff[i].readCompletionInfo.parameter = NULL;
        }
    }

}/* end releaseResources */

/****************************************************************************************************/
//
//		Method:		AppleUSBCDCEEM::USBTransmitPacket
//
//		Inputs:		packet - the packet
//
//		Outputs:	Return code - kIOReturnSuccess (transmit started), everything else (it didn't)
//
//		Desc:		Set up and then transmit the packet.
//
/****************************************************************************************************/

IOReturn AppleUSBCDCEEM::USBTransmitPacket(struct mbuf *packet)
{
    UInt32		numbufs;			// number of mbufs for this packet
    struct mbuf		*m;				// current mbuf
    UInt32		total_pkt_length = 0;
    UInt32		rTotal = 0;
    IOReturn		ior = kIOReturnSuccess;
    UInt32		indx;
    bool		gotBuffer = false;
	
    XTRACE(this, 0, packet, "USBTransmitPacket");
			
	// Count the number of mbufs in this packet
        
    m = packet;
    while (m)
    {
        total_pkt_length += m->m_len;
        numbufs++;
	m = m->m_next;
    }
    
    XTRACE(this, total_pkt_length, numbufs, "USBTransmitPacket - Total packet length and Number of mbufs");
    
//    if (total_pkt_length > gControlDriver->fMax_Block_Size)
	if (total_pkt_length > fMax_Block_Size)
    {
        XTRACE(this, 0, 0, "USBTransmitPacket - Bad packet size");	// Note for now and revisit later
		fpNetStats->outputErrors++;
        return kIOReturnInternalError;
    }
    
    if (fBufferPoolLock)
    {
        IOLockLock(fBufferPoolLock);
    }
    
        // Get an ouput buffer (use the hint first then if that's not available look for one)
    
    indx = fOutPoolIndex;
    if (!fPipeOutBuff[indx].avail)
    {
        for (indx=0; indx<fOutBufPool; indx++)
        {
            if (fPipeOutBuff[indx].avail)
            {
                fOutPoolIndex = indx;
                gotBuffer = true;
                break;
            }
        }
        if (!gotBuffer)
        {
            XTRACE(this, fOutBufPool, fOutPoolIndex, "USBTransmitPacket - Output buffer unavailable");
			fpNetStats->outputErrors++;
            if (fBufferPoolLock)
            {
                IOLockUnlock(fBufferPoolLock);
            }
            return kIOReturnInternalError;
        }
    }
    fOutPoolIndex++;
    if (fOutPoolIndex >= fOutBufPool)
    {
        fOutPoolIndex = 0;
    }
    
    if (fBufferPoolLock)
    {
        IOLockUnlock(fBufferPoolLock);
    }

        // Start filling in the send buffer

    m = packet;							// start with the first mbuf of the packet
    rTotal = sizeof(EEMPacketHeader);   // running total - start passed the EEM header
    do
    {  
        if (m->m_len == 0)					// Ignore zero length mbufs
            continue;
        
        bcopy(mtod(m, unsigned char *), &fPipeOutBuff[indx].pipeOutBuffer[rTotal], m->m_len);
        rTotal += m->m_len;
        
    } while ((m = m->m_next) != 0);
    
    LogData(kDataOut, rTotal, fPipeOutBuff[indx].pipeOutBuffer);
	
    fPipeOutBuff[indx].m = packet;
    fPipeOutBuff[indx].writeCompletionInfo.parameter = (void *)indx;
    fPipeOutBuff[indx].pipeOutMDP->setLength(rTotal);
    ior = fOutPipe->Write(fPipeOutBuff[indx].pipeOutMDP, &fPipeOutBuff[indx].writeCompletionInfo);
    if (ior != kIOReturnSuccess)
    {
        XTRACE(this, 0, ior, "USBTransmitPacket - Write failed");
        if (ior == kIOUSBPipeStalled)
        {
            fOutPipe->Reset();
            ior = fOutPipe->Write(fPipeOutBuff[indx].pipeOutMDP, &fPipeOutBuff[indx].writeCompletionInfo);
            if (ior != kIOReturnSuccess)
            {
                XTRACE(this, 0, ior, "USBTransmitPacket - Write really failed");
				fpNetStats->outputErrors++;
                return ior;
            }
        }
    }
        
	fpNetStats->outputPackets++;
    
    return ior;

}/* end USBTransmitPacket */

/****************************************************************************************************/
//
//		Method:		AppleUSBCDCEEM::clearPipeStall
//
//		Inputs:		thePipe - the pipe
//
//		Outputs:	
//
//		Desc:		Clear a stall on the specified pipe.
//
/****************************************************************************************************/

IOReturn AppleUSBCDCEEM::clearPipeStall(IOUSBPipe *thePipe)
{
    IOReturn 	rtn = kIOReturnSuccess;
    
    XTRACE(this, 0, thePipe, "clearPipeStall");
    
    rtn = thePipe->GetPipeStatus();
    if (rtn == kIOUSBPipeStalled)
    {
        rtn = thePipe->ClearPipeStall(true);
        if (rtn == kIOReturnSuccess)
        {
            XTRACE(this, 0, 0, "clearPipeStall - Successful");
        } else {
            XTRACE(this, 0, rtn, "clearPipeStall - Failed");
        }
    } else {
        XTRACE(this, 0, 0, "clearPipeStall - Pipe not stalled");
    }
    
    return rtn;

}/* end clearPipeStall */

/****************************************************************************************************/
//
//		Method:		AppleUSBCDCEEM::receivePacket
//
//		Inputs:		packet - the packet
//				size - Number of bytes in the packet
//
//		Outputs:	
//
//		Desc:		Build the mbufs and then send to the network stack.
//
/****************************************************************************************************/

void AppleUSBCDCEEM::receivePacket(UInt8 *packet, UInt32 size)
{
    struct mbuf		*m;
    UInt32		submit;
    
    XTRACE(this, 0, size, "receivePacket");
    
//    if (size > gControlDriver->fMax_Block_Size)
	if (size > fMax_Block_Size)
    {
        XTRACE(this, 0, 0, "receivePacket - Packet size error, packet dropped");
		fpNetStats->inputErrors++;
        return;
    }
    
    m = allocatePacket(size);
    if (m)
    {
        bcopy(packet, mtod(m, unsigned char *), size);
        submit = fNetworkInterface->inputPacket(m, size);
        XTRACE(this, 0, submit, "receivePacket - Packets submitted");
		fpNetStats->inputPackets++;
    } else {
        XTRACE(this, 0, 0, "receivePacket - Buffer allocation failed, packet dropped");
		fpNetStats->inputErrors++;
    }

}/* end receivePacket */

/****************************************************************************************************/
//
//		Method:		AppleUSBCDCEEM::message
//
//		Inputs:		type - message type
//				provider - my provider
//				argument - additional parameters
//
//		Outputs:	return Code - kIOReturnSuccess
//
//		Desc:		Handles IOKit messages. 
//
/****************************************************************************************************/

IOReturn AppleUSBCDCEEM::message(UInt32 type, IOService *provider, void *argument)
{
    UInt16	i;
    IOReturn	ior;
	
    XTRACE(this, 0, type, "message");
	
    switch (type)
    {
        case kIOMessageServiceIsTerminated:
            XTRACE(this, fReady, type, "message - kIOMessageServiceIsTerminated");
			
            if (fReady)
            {
                if (!fTerminate)		// Check if we're already being terminated
                { 
		    // NOTE! This call below depends on the hard coded path of this KEXT. Make sure
		    // that if the KEXT moves, this path is changed!
		    KUNCUserNotificationDisplayNotice(
			0,		// Timeout in seconds
			0,		// Flags (for later usage)
			"",		// iconPath (not supported yet)
			"",		// soundPath (not supported yet)
			"/System/Library/Extensions/IOUSBFamily.kext/Contents/PlugIns/AppleUSBCDCEEM.kext",	// localizationPath
			"Unplug Header",		// the header
			"Unplug Notice",		// the notice - look in Localizable.strings
			"OK"); 
                }
            }
            
            releaseResources();
            if (fDataInterface)	
            { 
                fDataInterface->close(this);
                fDataInterface->release();
                fDataInterface = NULL;
            }
            fTerminate = true;		// we're being terminated (unplugged)
            fLinkStatus = 0;		// and of course we're offline
            return kIOReturnSuccess;			
        case kIOMessageServiceIsSuspended: 	
            XTRACE(this, 0, type, "message - kIOMessageServiceIsSuspended");
            break;			
        case kIOMessageServiceIsResumed: 	
            XTRACE(this, 0, type, "message - kIOMessageServiceIsResumed");
            break;			
        case kIOMessageServiceIsRequestingClose: 
            XTRACE(this, 0, type, "message - kIOMessageServiceIsRequestingClose"); 
            break;
        case kIOMessageServiceWasClosed: 	
            XTRACE(this, 0, type, "message - kIOMessageServiceWasClosed"); 
            break;
        case kIOMessageServiceBusyStateChange: 	
            XTRACE(this, 0, type, "message - kIOMessageServiceBusyStateChange"); 
            break;
        case kIOUSBMessagePortHasBeenResumed: 	
            XTRACE(this, 0, type, "message - kIOUSBMessagePortHasBeenResumed");
            for (i=0; i<fInBufPool; i++)
            {
                if (fPipeInBuff[i].dead)			// If it's dead try and resurrect it
                {
                    ior = fInPipe->Read(fPipeInBuff[i].pipeInMDP, &fPipeInBuff[i].readCompletionInfo, NULL);
                    if (ior != kIOReturnSuccess)
                    {
                        XTRACE(this, 0, ior, "message - Read io error");
                    } else {
                        fPipeInBuff[i].dead = false;
                    }
                }
            }
            break;
        case kIOUSBMessageHubResumePort:
            XTRACE(this, 0, type, "message - kIOUSBMessageHubResumePort");
            break;
        default:
            XTRACE(this, 0, type, "message - unknown message"); 
            break;
    }
    
    return kIOReturnUnsupported;
    
}/* end message */