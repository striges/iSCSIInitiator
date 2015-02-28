/*!
 * @author		Nareg Sinenian
 * @file		iSCSIKernelInterface.c
 * @version		1.0
 * @copyright	(c) 2013-2015 Nareg Sinenian. All rights reserved.
 */

#include "iSCSIKernelInterface.h"
#include "iSCSIPDUUser.h"
#include <IOKit/IOKitLib.h>

static io_service_t service;
static io_connect_t connection;

/*! Opens a connection to the iSCSI initiator.  A connection must be
 *  successfully opened before any of the supporting functions below can be
 *  called. */
kern_return_t iSCSIKernelInitialize()
{
    kern_return_t kernResult;
     	
	// Create a dictionary to match iSCSIkext
	CFMutableDictionaryRef matchingDict = NULL;
	matchingDict = IOServiceMatching("com_NSinenian_iSCSIVirtualHBA");
    
    service = IOServiceGetMatchingService(kIOMasterPortDefault,matchingDict);
    
	// Check to see if the driver was found in the I/O registry
	if(service == IO_OBJECT_NULL)
	{
		return kIOReturnNotFound;
	}
    
	// Using the service handle, open a connection
	kernResult = IOServiceOpen(service,mach_task_self(),0,&connection);
	
	if(kernResult != kIOReturnSuccess) {
        IOObjectRelease(service);
        return kIOReturnNotFound;
    }
    return IOConnectCallScalarMethod(connection,kiSCSIOpenInitiator,0,0,0,0);
}

/*! Closes a connection to the iSCSI initiator. */
kern_return_t iSCSIKernelCleanUp()
{
    kern_return_t kernResult =
        IOConnectCallScalarMethod(connection,kiSCSICloseInitiator,0,0,0,0);
    
	// Clean up (now that we have a connection we no longer need the object)
    IOObjectRelease(service);
    IOServiceClose(connection);
    
    return kernResult;
}

/*! Allocates a new iSCSI session in the kernel and creates an associated
 *  connection to the target portal. Additional connections may be added to the
 *  session by calling iSCSIKernelCreateConnection().
 *  @param targetName the name of the target, or NULL if discovery session.
 *  @param domain the IP domain (e.g., AF_INET or AF_INET6).
 *  @param targetAddress the BSD socket structure used to identify the target.
 *  @param hostAddress the BSD socket structure used to identify the host. This
 *  specifies the interface that the connection will be bound to.
 *  @param sessionId the session identifier for the new session (returned).
 *  @param connectionId the identifier of the new connection (returned).
 *  @return An error code if a valid session could not be created. */
errno_t iSCSIKernelCreateSession(const char * targetName,
                                 int domain,
                                 const struct sockaddr * targetAddress,
                                 const struct sockaddr * hostAddress,
                                 SID * sessionId,
                                 CID * connectionId)
{
    // Check parameters
    if(!targetAddress || !hostAddress || !sessionId || !connectionId)
        return EINVAL;
    
    // Tell the kernel to drop this session and all of its related resources
    const UInt32 inputCnt = 1;
    const UInt64 input = domain;
    
    const UInt32 inputStructCnt = 2;
    
    size_t sockaddr_len = sizeof(struct sockaddr);
    size_t targetNameSize = sizeof(targetName)/sizeof(char);

    size_t inputBufferSize = inputStructCnt*sockaddr_len + targetNameSize;

    // Pack the targetAddress, hostAddress, targetName and connectionName into
    // a single buffer in that order.  The strings targetName and connectionName
    // are NULL-terminated C strings (the NULL character is copied)
    void * inputBuffer = malloc(inputBufferSize);

    memcpy(inputBuffer,targetAddress,sockaddr_len);
    memcpy(inputBuffer+sockaddr_len,hostAddress,sockaddr_len);
    
    // For discovery sessions target name is left blank (NULL)
    if(targetName)
        memcpy(inputBuffer+2*sockaddr_len,targetName,targetNameSize);
    
    const UInt32 expOutputCnt = 3;
    UInt64 output[expOutputCnt];
    UInt32 outputCnt = expOutputCnt;
    
    if(IOConnectCallMethod(connection,kiSCSICreateSession,&input,inputCnt,
                           inputBuffer,inputBufferSize,
                           output,&outputCnt,0,0) == kIOReturnSuccess)
    {
        if(outputCnt == expOutputCnt)
        {
            *sessionId    = (UInt16)output[1];
            *connectionId = (UInt32)output[2];
            return (UInt32)output[0];
        }
    }
    
    // Else we couldn't allocate a connection; quit
    return EINVAL;
}

/*! Releases an iSCSI session, including all connections associated with that
 *  session.
 *  @param sessionId the session qualifier part of the ISID. */
void iSCSIKernelReleaseSession(SID sessionId)
{
    // Check parameters
    if(sessionId == kiSCSIInvalidSessionId)
        return;

    // Tell the kernel to drop this session and all of its related resources
    const UInt32 inputCnt = 1;
    UInt64 input = sessionId;
    
    IOConnectCallScalarMethod(connection,kiSCSIReleaseSession,&input,inputCnt,0,0);
}

/*! Sets options associated with a particular connection.
 *  @param sessionId the qualifier part of the ISID (see RFC3720).
 *  @param options the options to set.
 *  @return error code indicating result of operation. */
errno_t iSCSIKernelSetSessionOptions(SID sessionId,
                                     iSCSISessionOptions * options)
{
    // Check parameters
    if(sessionId == kiSCSIInvalidSessionId || !options)
        return EINVAL;
    
    const UInt32 inputCnt = 1;
    const UInt64 input = sessionId;
    
    if(IOConnectCallMethod(connection,kiSCSISetSessionOptions,&input,inputCnt,
                           options,sizeof(struct iSCSISessionOptions),0,0,0,0) == kIOReturnSuccess)
    {
        return 0;
    }
    return EIO;
}

/*! Gets options associated with a particular connection.
 *  @param sessionId the qualifier part of the ISID (see RFC3720).
 *  @param options the options to get.  The user of this function is
 *  responsible for allocating and freeing the options struct.
 *  @return error code indicating result of operation. */
errno_t iSCSIKernelGetSessionOptions(SID sessionId,
                                     iSCSISessionOptions * options)
{
    // Check parameters
    if(sessionId == kiSCSIInvalidSessionId || !options)
        return EINVAL;
    
    const UInt32 inputCnt = 1;
    const UInt64 input = sessionId;
    size_t optionsSize = sizeof(struct iSCSISessionOptions);

    if(IOConnectCallMethod(connection,kiSCSIGetSessionOptions,&input,inputCnt,0,0,0,0,
                           options,&optionsSize) == kIOReturnSuccess)
    {
        return 0;
    }
    return EIO;
}

/*! Allocates an additional iSCSI connection for a particular session.
 *  @param sessionId the session to create a new connection for.
 *  @param domain the IP domain (e.g., AF_INET or AF_INET6).
 *  @param targetAddress the BSD socket structure used to identify the target.
 *  @param hostAddress the BSD socket structure used to identify the host. This
 *  specifies the interface that the connection will be bound to.
 *  @param connectionId the identifier of the new connection (returned).
 *  @return An error code if a valid connection could not be created. */
errno_t iSCSIKernelCreateConnection(SID sessionId,
                                    int domain,
                                    const struct sockaddr * targetAddress,
                                    const struct sockaddr * hostAddress,
                                    CID * connectionId)
{
    // Check parameters
    if(!targetAddress || !hostAddress || sessionId == kiSCSIInvalidSessionId)
        return kiSCSIInvalidConnectionId;
    
    // Tell the kernel to drop this session and all of its related resources
    const UInt32 inputCnt = 2;
    const UInt64 inputs[] = {sessionId,domain};
    
    const UInt32 inputStructCnt = 2;
    const struct sockaddr addresses[] = {*targetAddress,*hostAddress};
    
    const UInt32 expOutputCnt = 2;
    UInt64 output[expOutputCnt];
    UInt32 outputCnt = expOutputCnt;
    
    if(IOConnectCallMethod(connection,kiSCSICreateConnection,inputs,inputCnt,
                           addresses,inputStructCnt*sizeof(struct sockaddr),
                           output,&outputCnt,0,0) == kIOReturnSuccess)
    {
        if(outputCnt == expOutputCnt)
        {
            *connectionId = (UInt32)output[1];
            return (UInt32)output[0];
        }
    }

    // Else we couldn't allocate a connection; quit
    return EINVAL;
}

/*! Frees a given
 iSCSI connection associated with a given session.
 *  The session should be logged out using the appropriate PDUs. */
void iSCSIKernelReleaseConnection(SID sessionId,CID connectionId)
{
    // Check parameters
    if(sessionId == kiSCSIInvalidSessionId || connectionId == kiSCSIInvalidConnectionId)
        return;

    // Tell kernel to drop this connection
    const UInt32 inputCnt = 2;
    UInt64 inputs[] = {sessionId,connectionId};
    
    IOConnectCallScalarMethod(connection,kiSCSIReleaseConnection,inputs,inputCnt,0,0);
}


/*! Sends data over a kernel socket associated with iSCSI.
 *  @param sessionId the qualifier part of the ISID (see RFC3720).
 *  @param connectionId the connection associated with the session.
 *  @param bhs the basic header segment to send over the connection.
 *  @param data the data segment of the PDU to send over the connection.
 *  @param length the length of the data block to send over the connection.
 *  @return error code indicating result of operation. */
errno_t iSCSIKernelSend(SID sessionId,
                        CID connectionId,
                        iSCSIPDUInitiatorBHS * bhs,
                        void * data,
                        size_t length)
{
    // Check parameters
    if(sessionId    == kiSCSIInvalidSessionId || !bhs || (!data && length > 0) ||
       connectionId == kiSCSIInvalidConnectionId)
        return EINVAL;
    
    // Setup input scalar array
    const UInt32 inputCnt = 2;
    const UInt64 inputs[] = {sessionId, connectionId};
    
    const UInt32 expOutputCnt = 1;
    UInt32 outputCnt = 1;
    UInt64 output;
    
    // Call kernel method to send (buffer) bhs and then data
    if(IOConnectCallStructMethod(connection,kiSCSISendBHS,bhs,
            sizeof(iSCSIPDUInitiatorBHS),NULL,NULL) != kIOReturnSuccess)
    {
        return EINVAL;
    }
    
    if(IOConnectCallMethod(connection,kiSCSISendData,inputs,inputCnt,
            data,length,&output,&outputCnt,NULL,NULL) == kIOReturnSuccess)
    {
        if(outputCnt == expOutputCnt)
            return (errno_t)output;
    }
    
    // Return -1 as the BSD socket API normally would if the kernel call fails
    return EINVAL;
}

/*! Receives data over a kernel socket associated with iSCSI.
 *  @param sessionId the qualifier part of the ISID (see RFC3720).
 *  @param connectionId the connection associated with the session.
 *  @param bhs the basic header segment received over the connection.
 *  @param data the data segment of the PDU received over the connection.
 *  @param length the length of the data block received.
 *  @return error code indicating result of operation. */
errno_t iSCSIKernelRecv(SID sessionId,
                        CID connectionId,
                        iSCSIPDUTargetBHS * bhs,
                        void * * data,
                        size_t * length)
{
    // Check parameters
    if(sessionId == kiSCSIInvalidSessionId || connectionId == kiSCSIInvalidConnectionId || !bhs)
        return EINVAL;
    
    // Setup input scalar array
    const UInt32 inputCnt = 2;
    UInt64 inputs[] = {sessionId,connectionId};
    
    const UInt32 expOutputCnt = 1;
    UInt32 outputCnt = 1;
    UInt64 output;
    
    size_t bhsLength = sizeof(iSCSIPDUTargetBHS);

    // Call kernel method to determine how much data there is to receive
    // The inputs are the sesssion qualifier and connection ID
    // The output is the size of the buffer we need to allocate to hold the data
    kern_return_t kernResult;
    
    kernResult = IOConnectCallMethod(connection,kiSCSIRecvBHS,inputs,inputCnt,NULL,0,
                                     &output,&outputCnt,bhs,&bhsLength);
    
    if(kernResult != kIOReturnSuccess || outputCnt != expOutputCnt || output != 0)
        return EIO;
    
    // Determine how much data to allocate for the data buffer
    *length = iSCSIPDUGetDataSegmentLength((iSCSIPDUCommonBHS *)bhs);
    
    // If no data, were done at this point
    if(*length == 0)
        return 0;
    
    *data = iSCSIPDUDataCreate(*length);
        
    if(*data == NULL)
        return EIO;
    
    // Call kernel method to get data from a receive buffer
    if(IOConnectCallMethod(connection,kiSCSIRecvData,inputs,inputCnt,NULL,0,
                           &output,&outputCnt,*data,length) == kIOReturnSuccess)
    {
        if(outputCnt == expOutputCnt && output == 0)
            return 0;
    }
    
    // At this point we failed, free the temporary buffer and quit with error
    iSCSIPDUDataRelease(data);
    return EIO;
}


/*! Sets options associated with a particular connection.
 *  @param sessionId the qualifier part of the ISID (see RFC3720).
 *  @param connectionId the connection associated with the session.
 *  @param options the options to set.
 *  @return error code indicating result of operation. */
errno_t iSCSIKernelSetConnectionOptions(SID sessionId,
                                        CID connectionId,
                                        iSCSIConnectionOptions * options)
{
    // Check parameters
    if(sessionId == kiSCSIInvalidSessionId ||
       connectionId == kiSCSIInvalidConnectionId || !options)
        return EINVAL;
    
    const UInt32 inputCnt = 2;
    const UInt64 inputs[] = {sessionId,connectionId};
    
    if(IOConnectCallMethod(connection,kiSCSISetConnectionOptions,inputs,inputCnt,
                           options,sizeof(struct iSCSIConnectionOptions),0,0,0,0) == kIOReturnSuccess)
    {
        return 0;
    }
    return EIO;
}

/*! Gets options associated with a particular connection.
 *  @param sessionId the qualifier part of the ISID (see RFC3720).
 *  @param connectionId the connection associated with the session.
 *  @param options the options to get.  The user of this function is
 *  responsible for allocating and freeing the options struct.
 *  @return error code indicating result of operation. */
errno_t iSCSIKernelGetConnectionOptions(SID sessionId,
                                        CID connectionId,
                                        iSCSIConnectionOptions * options)
{
    // Check parameters
    if(sessionId == kiSCSIInvalidSessionId ||
       connectionId     == kiSCSIInvalidConnectionId || !options)
        return EINVAL;
    
    const UInt32 inputCnt = 2;
    const UInt64 inputs[] = {sessionId,connectionId};

    size_t optionsSize = sizeof(struct iSCSIConnectionOptions);
    
    if(IOConnectCallMethod(connection,kiSCSIGetConnectionOptions,inputs,inputCnt,0,0,0,0,
                           options,&optionsSize) == kIOReturnSuccess)
    {
        return 0;
    }
    return EIO;
}

/*! Activates an iSCSI connection associated with a session.
 *  @param sessionId session associated with connection to activate.
 *  @param connectionId  connection to activate.
 *  @return error code inidicating result of operation. */
errno_t iSCSIKernelActivateConnection(SID sessionId,CID connectionId)
{
    // Check parameters
    if(sessionId == kiSCSIInvalidSessionId || connectionId == kiSCSIInvalidConnectionId)
        return EINVAL;
    
    // Tell kernel to drop this connection
    const UInt32 inputCnt = 2;
    UInt64 inputs[] = {sessionId,connectionId};
    
    UInt64 output;
    UInt32 outputCnt = 1;
    const UInt32 expOutputCnt = 1;
    
    if(IOConnectCallScalarMethod(connection,kiSCSIActivateConnection,
                                 inputs,inputCnt,&output,&outputCnt) == kIOReturnSuccess)
    {
        if(outputCnt == expOutputCnt)
            return (errno_t)output;
    }
    return EINVAL;
}

/*! Activates all iSCSI connections associated with a session.
 *  @param sessionId session associated with connection to activate.
 *  @return error code inidicating result of operation. */
errno_t iSCSIKernelActivateAllConnections(SID sessionId)
{
    // Check parameters
    if(sessionId == kiSCSIInvalidSessionId)
        return EINVAL;
    
    const UInt32 inputCnt = 1;
    UInt64 input = sessionId;
    
    UInt64 output;
    UInt32 outputCnt = 1;
    const UInt32 expOutputCnt = 1;
    
    if(IOConnectCallScalarMethod(connection,kiSCSIActivateAllConnections,
                                 &input,inputCnt,&output,&outputCnt) == kIOReturnSuccess)
    {
        if(outputCnt == expOutputCnt)
            return (errno_t)output;
    }
    return EINVAL;
}


/*! Dectivates an iSCSI connection associated with a session.
 *  @param sessionId session associated with connection to deactivate.
 *  @param connectionId  connection to deactivate.
 *  @return error code inidicating result of operation. */
errno_t iSCSIKernelDeactivateConnection(SID sessionId,CID connectionId)
{
    // Check parameters
    if(sessionId == kiSCSIInvalidSessionId || connectionId == kiSCSIInvalidConnectionId)
        return EINVAL;
    
    // Tell kernel to drop this connection
    const UInt32 inputCnt = 2;
    UInt64 inputs[] = {sessionId,connectionId};
    
    UInt64 output;
    UInt32 outputCnt = 1;
    const UInt32 expOutputCnt = 1;
    
    if(IOConnectCallScalarMethod(connection,kiSCSIDeactivateConnection,
                                 inputs,inputCnt,&output,&outputCnt) == kIOReturnSuccess)
    {
        if(outputCnt == expOutputCnt)
            return (errno_t)output;
    }
    return EINVAL;
}

/*! Dectivates all iSCSI sessions associated with a session.
 *  @param sessionId session associated with connections to deactivate.
 *  @return error code inidicating result of operation. */
errno_t iSCSIKernelDeactivateAllConnections(SID sessionId)
{
    // Check parameters
    if(sessionId == kiSCSIInvalidSessionId)
        return EINVAL;
    
    const UInt32 inputCnt = 1;
    UInt64 input = sessionId;
    
    UInt64 output;
    UInt32 outputCnt = 1;
    const UInt32 expOutputCnt = 1;
    
    if(IOConnectCallScalarMethod(connection,kiSCSIDeactivateAllConnections,
                                 &input,inputCnt,&output,&outputCnt) == kIOReturnSuccess)
    {
        if(outputCnt == expOutputCnt)
            return (errno_t)output;
    }
    return EINVAL;
}

/*! Gets the first connection (the lowest connectionId) for the
 *  specified session.
 *  @param sessionId obtain an connectionId for this session.
 *  @param connectionId the identifier of the connection.
 *  @return error code indicating result of operation. */
errno_t iSCSIKernelGetConnection(SID sessionId,CID * connectionId)
{
    // Check parameters
    if(sessionId == kiSCSIInvalidSessionId || !connectionId)
        return EINVAL;
    
    const UInt32 inputCnt = 1;
    UInt64 input = sessionId;
    
    const UInt32 expOutputCnt = 2;
    UInt64 output[expOutputCnt];
    UInt32 outputCnt = expOutputCnt;
    
    if(IOConnectCallScalarMethod(connection,kiSCSIGetConnection,
                                 &input,inputCnt,output,&outputCnt) == kIOReturnSuccess)
    {
        if(outputCnt == expOutputCnt)
        {
            *connectionId = (UInt32)output[1];
            return (errno_t)output[0];
        }
    }
    return EINVAL;

}

/*! Gets the connection count for the specified session.
 *  @param sessionId obtain the connection count for this session.
 *  @param numConnections the connection count.
 *  @return error code indicating result of operation. */
errno_t iSCSIKernelGetNumConnections(SID sessionId,UInt32 * numConnections)
{
    // Check parameters
    if(sessionId == kiSCSIInvalidSessionId || !numConnections)
        return EINVAL;
    
    const UInt32 inputCnt = 1;
    UInt64 input = sessionId;
    
    const UInt32 expOutputCnt = 2;
    UInt64 output[expOutputCnt];
    UInt32 outputCnt = expOutputCnt;
    
    if(IOConnectCallScalarMethod(connection,kiSCSIGetNumConnections,
                                 &input,inputCnt,output,&outputCnt) == kIOReturnSuccess)
    {
        if(outputCnt == expOutputCnt)
        {
            *numConnections = (UInt32)output[1];
            return (errno_t)output;
        }
    }
    return EINVAL;
}

/*! Looks up the session identifier associated with a particular target name.
 *  @param targetName the IQN name of the target (e.q., iqn.2015-01.com.example)
 *  @param sessionId the session identifier.
 *  @return error code indicating result of operation. */
errno_t iSCSIKernelGetSessionIdFromTargetName(const char * targetName,SID * sessionId)
{
    if(!targetName || !sessionId)
        return EINVAL;
    
    const UInt32 expOutputCnt = 2;
    UInt64 output[expOutputCnt];
    UInt32 outputCnt = expOutputCnt;
    
    if(IOConnectCallMethod(connection,kiSCSIGetSessionIdFromTargetName,0,0,
                           targetName,sizeof(targetName)/sizeof(char),
                           output,&outputCnt,0,0) == kIOReturnSuccess)
    {
        if(outputCnt == expOutputCnt)
        {
            *sessionId = (UInt16)output[1];
            return (errno_t)output[0];
        }
    }
    
    return EINVAL;
}

/*! Looks up the connection identifier associated with a particular connection address.
 *  @param sessionId the session identifier.
 *  @param address the name used when adding the connection (e.g., IP or DNS).
 *  @param connectionId the associated connection identifier.
 *  @return error code indicating result of operation. */
errno_t iSCSIKernelGetConnectionIdFromAddress(SID sessionId,
                                              const char * address,
                                              CID * connectionId)
{
    if(!address || !sessionId || !connectionId)
        return EINVAL;
    
    // Convert address string to an address structure
    struct sockaddr addr;
    
    const UInt32 inputCnt = 1;
    UInt64 input = sessionId;
    
    const UInt32 expOutputCnt = 2;
    UInt64 output[expOutputCnt];
    UInt32 outputCnt = expOutputCnt;
    
    if(IOConnectCallMethod(connection,kiSCSIGetConnectionIdFromAddress,&input,inputCnt,
                           address,sizeof(address)/sizeof(char),
                           output,&outputCnt,0,0) == kIOReturnSuccess)
    {
        if(outputCnt == expOutputCnt)
        {
            *connectionId = (UInt16)output[1];
            return (errno_t)output[0];
        }
    }
    
    inet_addr
    return EINVAL;
}

/*! Gets an array of session identifiers for each session.
 *  @param sessionIds an array of session identifiers.  This MUST be large
 *  enough to hold the maximum number of sessions (kiSCSIMaxSessions).
 *  @param sessionCount number of session identifiers.
 *  @return error code indicating result of operation. */
errno_t iSCSIKernelGetSessionIds(UInt16 * sessionIds,
                                 UInt16 * sessionCount)
{
    if(!sessionIds || !sessionCount)
        return EINVAL;
    
    const UInt32 expOutputCnt = 2;
    UInt64 output[expOutputCnt];
    UInt32 outputCnt = expOutputCnt;
    
    *sessionCount = 0;
    size_t outputStructSize = sizeof(UInt16)*kiSCSIMaxSessions;;
    
    if(IOConnectCallMethod(connection,kiSCSIGetSessionIds,0,0,0,0,
                           output,&outputCnt,sessionIds,&outputStructSize) == kIOReturnSuccess)
    {
        if(outputCnt == expOutputCnt)
        {
            *sessionCount = (UInt32)output[1];
            return (errno_t)output[0];
        }
    }
    
    return EINVAL;
}

/*! Gets an array of connection identifiers for each session.
 *  @param sessionId session identifier.
 *  @param connectionIds an array of connection identifiers for the session.
 *  @param connectionCount number of connection identifiers.
 *  @return error code indicating result of operation. */
errno_t iSCSIKernelGetConnectionIds(SID sessionId,
                                    UInt32 * connectionIds,
                                    UInt32 * connectionCount)
{
    if(sessionId == kiSCSIInvalidSessionId || !connectionIds || !connectionCount)
        return EINVAL;
    
    const UInt32 inputCnt = 1;
    UInt64 input = sessionId;
    
    const UInt32 expOutputCnt = 2;
    UInt64 output[expOutputCnt];
    UInt32 outputCnt = expOutputCnt;
    
    *connectionCount = 0;
    size_t outputStructSize = sizeof(UInt32)*kiSCSIMaxConnectionsPerSession;

    if(IOConnectCallMethod(connection,kiSCSIGetConnectionIds,&input,inputCnt,0,0,
                           output,&outputCnt,connectionIds,&outputStructSize) == kIOReturnSuccess)
    {
        if(outputCnt == expOutputCnt)
        {
            *connectionCount = (UInt32)output[1];
            return (errno_t)output[0];
        }
    }
    
    return EINVAL;
}

