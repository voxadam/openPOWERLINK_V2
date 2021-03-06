/**
********************************************************************************
\file   hrestimer-i210.c

\brief  High-resolution timer module for Intel I210 Ethernet controller

This module is an implementation of the high-resolution timer for Linux
kernel-space which uses the hardware timer located in the Intel I210 Ethernet
controller.

\ingroup module_hrestimer
*******************************************************************************/

/*------------------------------------------------------------------------------
Copyright (c) 2013, Kalycito Infotech Private Limited
Copyright (c) 2012, SYSTEC electronic GmbH
Copyright (c) 2014, Bernecker+Rainer Industrie-Elektronik Ges.m.b.H. (B&R)
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the copyright holders nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL COPYRIGHT HOLDERS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
------------------------------------------------------------------------------*/

//------------------------------------------------------------------------------
// includes
//------------------------------------------------------------------------------
#include <oplk/oplkinc.h>
#include <kernel/hrestimer.h>
#include <oplk/benchmark.h>

#include <kernel/edrv.h>

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/hrtimer.h>

//============================================================================//
//            G L O B A L   D E F I N I T I O N S                             //
//============================================================================//

//------------------------------------------------------------------------------
// const defines
//------------------------------------------------------------------------------

#define TIMER_COUNT             1
#define TIMER_MIN_VAL_SINGLE    5000             /* min 5us */
#define TIMER_MIN_VAL_CYCLE     100000           /* min 100us */

// TracePoint support for realtime-debugging
#ifdef _DBG_TRACE_POINTS_
void TgtDbgSignalTracePoint (UINT8 bTracePointNumber_p);
void TgtDbgPostTraceValue (UINT32 dwTraceValue_p);
#define TGT_DBG_SIGNAL_TRACE_POINT(p)   TgtDbgSignalTracePoint(p)
#define TGT_DBG_POST_TRACE_VALUE(v)     TgtDbgPostTraceValue(v)
#else
#define TGT_DBG_SIGNAL_TRACE_POINT(p)
#define TGT_DBG_POST_TRACE_VALUE(v)
#endif
#define HRT_DBG_POST_TRACE_VALUE(Event_p, uiNodeId_p, wErrorCode_p) \
            TGT_DBG_POST_TRACE_VALUE((0xE << 28) |                  \
                                     (Event_p << 24) |              \
                                     (uiNodeId_p << 16) |           \
                                     wErrorCode_p)

#define TIMERHDL_MASK           0x0FFFFFFF
#define TIMERHDL_SHIFT          28
#define HDL_TO_IDX(Hdl)         ((Hdl >> TIMERHDL_SHIFT) - 1)
#define HDL_INIT(Idx)           ((Idx + 1) << TIMERHDL_SHIFT)
#define HDL_INC(Hdl)            (((Hdl + 1) & TIMERHDL_MASK) \
                                 | (Hdl & ~TIMERHDL_MASK))

//------------------------------------------------------------------------------
// module global vars
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
// global function prototypes
//------------------------------------------------------------------------------
tOplkError edrv_getMacTime(UINT64* pCurtime_p);
void edrv_setCyclicFrequency(UINT32 frequency);
tOplkError edrv_startTimer(tTimerHdl* pTimerHdl_p, UINT32 frequency_p);
tOplkError edrv_stopTimer(tTimerHdl* pTimerHdl_p);
tOplkError edrv_restartTimer(tTimerHdl* pTimerHdl_p);
tOplkError edrv_registerHresCallback(tHresCallback pfnHighResCb_p);

//============================================================================//
//          P R I V A T E   D E F I N I T I O N S                             //
//============================================================================//

//------------------------------------------------------------------------------
// const defines
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
// local types
//------------------------------------------------------------------------------

/**
\brief  High-resolution timer info

The structure provides information for a high-resolution timer.
*/
typedef struct
{
    tTimerEventArg       eventArg;          ///< Argument for timer event
    tTimerkCallback      pfnCallback;       ///< Timer callback function
    BOOL                 fContinuously;     ///< Determines if it is a continuous or one-shot timer
    ULONGLONG            period;            ///< The timer period
} tHresTimerInfo;

/**
\brief  High-resolution timer instance

The structure defines a high-resolution timer module instance.
*/
typedef struct
{
    tHresTimerInfo      aTimerInfo[TIMER_COUNT];    ///< Array with timer information for a set of timers
} tHresTimerInstance;

//------------------------------------------------------------------------------
// module local vars
//------------------------------------------------------------------------------
static tHresTimerInstance    hresTimerInstance_l;

//------------------------------------------------------------------------------
// local function prototypes
//------------------------------------------------------------------------------
static void timerCallback(tTimerHdl* pTimerHdl_p);

//============================================================================//
//            P U B L I C   F U N C T I O N S                                 //
//============================================================================//

//------------------------------------------------------------------------------
/**
\brief    Initialize high-resolution timer module

The function initializes the high-resolution timer module

\return Returns a tOplkError error code.

\ingroup module_hrestimer
*/
//------------------------------------------------------------------------------
tOplkError hrestimer_init(void)
{
    return hrestimer_addInstance();
}

//------------------------------------------------------------------------------
/**
\brief    Add instance of high-resolution timer module

The function adds an instance of the high-resolution timer module

\return Returns a tOplkError error code.

\ingroup module_hrestimer
*/
//------------------------------------------------------------------------------
tOplkError hrestimer_addInstance(void)
{
    OPLK_MEMSET(&hresTimerInstance_l, 0, sizeof(hresTimerInstance_l));
    return kErrorOk;
}

//------------------------------------------------------------------------------
/**
\brief    Delete instance of high-resolution timer module

The function deletes an instance of the high-resolution timer module

\return Returns a tOplkError error code.

\ingroup module_hrestimer
*/
//------------------------------------------------------------------------------
tOplkError hrestimer_delInstance(void)
{
    tHresTimerInfo*     pTimerInfo;
    tOplkError          ret = kErrorOk;
    UINT                index;

    for (index = 0; index < TIMER_COUNT; index++)
    {
        pTimerInfo = &hresTimerInstance_l.aTimerInfo[index];
        pTimerInfo->pfnCallback = NULL;

        ret = edrv_stopTimer(&pTimerInfo->eventArg.timerHdl);
        if (ret != kErrorOk)
            break;
        pTimerInfo->eventArg.timerHdl = 0;
    }
    return ret;
}

//------------------------------------------------------------------------------
/**
\brief    Modify a high-resolution timer

The function modifies the timeout of the timer with the specified handle.
If the handle the pointer points to is zero, the timer must be created first.
If it is not possible to stop the old timer, this function always assures that
the old timer does not trigger the callback function with the same handle as
the new timer. That means the callback function must check the passed handle
with the one returned by this function. If these are unequal, the call can be
discarded.

\param  pTimerHdl_p     Pointer to timer handle.
\param  time_p          Relative timeout in [ns].
\param  pfnCallback_p   Callback function, which is called when the timer has
                        expired. (The function is called mutually exclusive with
                        the Edrv callback functions (Rx and Tx)).
\param  argument_p      User-specific argument
\param  fContinue_p     If TRUE, callback function will be called continuously.
                        Otherwise, it is a one-shot timer.

\return Returns a tOplkError error code.

\ingroup module_hrestimer
*/
//------------------------------------------------------------------------------
tOplkError hrestimer_modifyTimer(tTimerHdl* pTimerHdl_p, ULONGLONG time_p,
                                 tTimerkCallback pfnCallback_p, ULONG argument_p,
                                 BOOL fContinue_p)
{
    tOplkError          ret = kErrorOk;
    UINT                index;
    tHresTimerInfo*     pTimerInfo;
    UINT32              timerFreq;

    if (pTimerHdl_p == NULL )
        return kErrorTimerInvalidHandle;

    if (*pTimerHdl_p == 0)
    {   // no timer created yet

        // search free timer info structure
        pTimerInfo = &hresTimerInstance_l.aTimerInfo[0];
        for (index = 0; index < TIMER_COUNT; index++, pTimerInfo++)
        {
            if (pTimerInfo->eventArg.timerHdl == 0)
                break;      // free structure found
        }

        if (index >= TIMER_COUNT)
            return kErrorTimerNoTimerCreated;    // no free structure found

        pTimerInfo->eventArg.timerHdl = HDL_INIT(index);
    }
    else
    {
        index = HDL_TO_IDX(*pTimerHdl_p);
        if (index >= TIMER_COUNT)
            return kErrorTimerInvalidHandle;       // invalid handle

        pTimerInfo = &hresTimerInstance_l.aTimerInfo[index];
    }

    /* increment timer handle
     * (if timer expires right after this statement, the user
     * would detect an unknown timer handle and discard it) */
    pTimerInfo->eventArg.timerHdl = HDL_INC(pTimerInfo->eventArg.timerHdl);
    *pTimerHdl_p = pTimerInfo->eventArg.timerHdl;

    if (fContinue_p != FALSE)
    {
        if (time_p < TIMER_MIN_VAL_CYCLE)
            time_p = TIMER_MIN_VAL_CYCLE;
    }
    else
    {
        if (time_p < TIMER_MIN_VAL_SINGLE)
            time_p = TIMER_MIN_VAL_SINGLE;
    }

    pTimerInfo->eventArg.argument.value = argument_p;
    pTimerInfo->pfnCallback = pfnCallback_p;
    pTimerInfo->period = time_p;
    pTimerInfo->fContinuously = fContinue_p;

    ret = edrv_registerHresCallback(timerCallback);
    if (ret != kErrorOk)
        return ret;

    timerFreq = (UINT32)(time_p);
    ret = edrv_startTimer(&pTimerInfo->eventArg.timerHdl, timerFreq);

    return ret;
}

//------------------------------------------------------------------------------
/**
\brief    Delete a high-resolution timer

The function deletes a created high-resolution timer. The timer is specified
by its timer handle. After deleting, the handle is reset to zero.

\param  pTimerHdl_p     Pointer to timer handle.

\return Returns a tOplkError error code.

\ingroup module_hrestimer
*/
//------------------------------------------------------------------------------
tOplkError hrestimer_deleteTimer(tTimerHdl* pTimerHdl_p)
{
    tOplkError          ret = kErrorOk;
    UINT                index;
    tHresTimerInfo*     pTimerInfo;

    if (pTimerHdl_p == NULL )
        return kErrorTimerInvalidHandle;

    if (*pTimerHdl_p == 0)
    {   // no timer created yet
        return ret;
    }
    else
    {
        index = HDL_TO_IDX(*pTimerHdl_p);
        if (index >= TIMER_COUNT)
            return kErrorTimerInvalidHandle;       // invalid handle

        pTimerInfo = &hresTimerInstance_l.aTimerInfo[index];
        if (pTimerInfo->eventArg.timerHdl != *pTimerHdl_p)
            return ret;     // invalid handle
    }

    edrv_stopTimer(pTimerHdl_p);

    *pTimerHdl_p = 0;
    pTimerInfo->eventArg.timerHdl = 0;
    pTimerInfo->pfnCallback = NULL;

    return ret;
}


//============================================================================//
//            P R I V A T E   F U N C T I O N S                               //
//============================================================================//
/// \name Private Functions
/// \{

//------------------------------------------------------------------------------
/**
\brief    Timer callback function

The function provides the timer callback function which is called when a timer
has expired.

\param  pTimerHdl_p     Pointer to hrtimer struct of the expired timer

\return Returns a hrtimer_restart value
*/
//------------------------------------------------------------------------------
static void timerCallback(tTimerHdl* pTimerHdl_p)
{
    UINT            index;
    tTimerHdl       orgTimerHdl;
    tHresTimerInfo* pTimerInfo;

    index = HDL_TO_IDX(*pTimerHdl_p);
    if (index >= TIMER_COUNT)
        return;      // invalid handle

    pTimerInfo = &hresTimerInstance_l.aTimerInfo[index];

    orgTimerHdl = *pTimerHdl_p;
    if (pTimerInfo->pfnCallback != NULL )
    {
        pTimerInfo->pfnCallback(&pTimerInfo->eventArg);
    }

    if (orgTimerHdl != pTimerInfo->eventArg.timerHdl)
        return;

    if (pTimerInfo->fContinuously)
    {
        edrv_restartTimer(pTimerHdl_p);
    }
    else
    {
        edrv_stopTimer(pTimerHdl_p);
    }
    return;
}

/// \}

