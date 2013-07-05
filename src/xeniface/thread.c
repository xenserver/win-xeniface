/* Copyright (c) Citrix Systems Inc.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, 
 * with or without modification, are permitted provided 
 * that the following conditions are met:
 * 
 * *   Redistributions of source code must retain the above 
 *     copyright notice, this list of conditions and the 
 *     following disclaimer.
 * *   Redistributions in binary form must reproduce the above 
 *     copyright notice, this list of conditions and the 
 *     following disclaimer in the documentation and/or other 
 *     materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND 
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, 
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF 
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE 
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR 
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, 
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR 
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, 
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING 
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF 
 * SUCH DAMAGE.
 */

#include <ntddk.h>
#include <util.h>

#include "thread.h"
#include "log.h"
#include "assert.h"

#define THREAD_POOL 'ERHT'

struct _XENIFACE_THREAD {
    XENIFACE_THREAD_FUNCTION  Function;
    PVOID                   Context;
    KEVENT                  Event;
    BOOLEAN                 Alerted;
    LONG                    References;
    PKTHREAD                Thread;
};

static FORCEINLINE PVOID
__ThreadAllocate(
    IN  ULONG   Length
    )
{
    return __AllocateNonPagedPoolWithTag(Length, THREAD_POOL);
}

static FORCEINLINE VOID
__ThreadFree(
    IN  PVOID   Buffer
    )
{
    __FreePoolWithTag(Buffer, THREAD_POOL);
}

static FORCEINLINE VOID
__ThreadWake(
    IN  PXENIFACE_THREAD  Thread
    )
{
    KeSetEvent(&Thread->Event, IO_NO_INCREMENT, FALSE);
}

VOID
ThreadWake(
    IN  PXENIFACE_THREAD  Thread
    )
{
    __ThreadWake(Thread);
}

static FORCEINLINE VOID
__ThreadAlert(
    IN  PXENIFACE_THREAD  Thread
    )
{
    Thread->Alerted = TRUE;
    __ThreadWake(Thread);
}

VOID
ThreadAlert(
    IN  PXENIFACE_THREAD  Thread
    )
{
    __ThreadAlert(Thread);
}

KSTART_ROUTINE  ThreadFunction;

VOID
ThreadFunction(
    IN  PVOID       Argument
    )
{
    PXENIFACE_THREAD  Self = Argument;
    NTSTATUS        status;

    status = Self->Function(Self, Self->Context);

    if (InterlockedDecrement(&Self->References) == 0)
        __ThreadFree(Self);

    PsTerminateSystemThread(status);
    // NOT REACHED
}

NTSTATUS
ThreadCreate(
    IN  XENIFACE_THREAD_FUNCTION  Function,
    IN  PVOID                   Context,
    OUT PXENIFACE_THREAD          *Thread
    )
{
    HANDLE                      Handle;
    NTSTATUS                    status;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    (*Thread) = __ThreadAllocate(sizeof (XENIFACE_THREAD));

	Warning("Create thread %p %p\n", Function, Thread);

    status = STATUS_NO_MEMORY;
    if (*Thread == NULL)
        goto fail1;

    (*Thread)->Function = Function;
    (*Thread)->Context = Context;
    (*Thread)->Alerted = FALSE;
    (*Thread)->References = 2; // One for us, one for the thread function

    KeInitializeEvent(&(*Thread)->Event, NotificationEvent, FALSE);

    status = PsCreateSystemThread(&Handle,
                                  STANDARD_RIGHTS_ALL | SPECIFIC_RIGHTS_ALL,
                                  NULL,
                                  NULL,
                                  NULL,
                                  ThreadFunction,
                                  *Thread);
    if (!NT_SUCCESS(status)) {
        --(*Thread)->References;    // Fake thread function termination
        goto fail2;
    }

    status = ObReferenceObjectByHandle(Handle,
                                       SYNCHRONIZE,
                                       *PsThreadType,
                                       KernelMode,
                                       &(*Thread)->Thread,
                                       NULL);
    if (!NT_SUCCESS(status))
        goto fail3;

    ZwClose(Handle);

    return STATUS_SUCCESS;

fail3:
    Error("fail3\n");

    __ThreadAlert(*Thread);
    ZwClose(Handle);

fail2:
    Error("fail2\n");

    if (InterlockedDecrement(&(*Thread)->References) == 0)
        __ThreadFree(*Thread);

    *Thread = NULL;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

PKEVENT
ThreadGetEvent(
    IN  PXENIFACE_THREAD  Thread
    )
{
    return &Thread->Event;
}

BOOLEAN
ThreadIsAlerted(
    IN  PXENIFACE_THREAD  Thread
    )
{
    return Thread->Alerted;
}

VOID
ThreadJoin(
    IN  PXENIFACE_THREAD  Thread
    )
{
    LONG                References;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);
    ASSERT3P(KeGetCurrentThread(), !=, Thread->Thread);

    (VOID) KeWaitForSingleObject(Thread->Thread,
                                 Executive,
                                 KernelMode,
                                 FALSE,
                                 NULL);

    References = InterlockedDecrement(&Thread->References);
    ASSERT3U(References, ==, 0);

    __ThreadFree(Thread);
}
