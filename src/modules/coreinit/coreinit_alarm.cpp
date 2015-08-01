#include "coreinit.h"
#include "coreinit_alarm.h"
#include "coreinit_core.h"
#include "coreinit_spinlock.h"
#include "coreinit_scheduler.h"
#include "coreinit_thread.h"
#include "coreinit_memheap.h"
#include "coreinit_time.h"
#include "coreinit_queue.h"
#include "interpreter.h"
#include "processor.h"

static OSSpinLock *
gAlarmLock;

static OSAlarmQueue *
gAlarmQueue[3];

const uint32_t
OSAlarm::Tag;

static BOOL
OSCancelAlarmNoLock(OSAlarm *alarm)
{
   if (!alarm->state == OSAlarmState::Set) {
      return FALSE;
   }

   alarm->state = OSAlarmState::Cancelled;
   alarm->nextFire = 0;
   alarm->period = 0;
   OSEraseFromQueue(static_cast<OSAlarmQueue*>(alarm->alarmQueue), alarm);
   return TRUE;
}

BOOL
OSCancelAlarm(OSAlarm *alarm)
{
   ScopedSpinLock lock(gAlarmLock);
   return OSCancelAlarmNoLock(alarm);
}

void
OSCancelAlarms(uint32_t alarmTag)
{
   ScopedSpinLock lock(gAlarmLock);

   // Cancel all alarms with matching alarmTag
   for (auto i = 0u; i < 3; ++i) {
      auto queue = gAlarmQueue[i];

      for (OSAlarm *alarm = queue->head; alarm; alarm = alarm->link.next) {
         if (alarm->alarmTag == alarmTag) {
            OSCancelAlarmNoLock(alarm);
         }
      }
   }
}

void
OSCreateAlarm(OSAlarm *alarm)
{
   OSCreateAlarmEx(alarm, nullptr);
}

void
OSCreateAlarmEx(OSAlarm *alarm, const char *name)
{
   memset(alarm, 0, sizeof(OSAlarm));
   alarm->tag = OSAlarm::Tag;
   alarm->name = name;
   OSInitThreadQueueEx(&alarm->threadQueue, alarm);
}

void *
OSGetAlarmUserData(OSAlarm *alarm)
{
   return alarm->userData;
}

void
OSInitAlarmQueue(OSAlarmQueue *queue)
{
   memset(queue, 0, sizeof(OSAlarmQueue));
   queue->tag = OSAlarmQueue::Tag;
}

BOOL
OSSetAlarm(OSAlarm *alarm, OSTime time, AlarmCallback callback)
{
   return OSSetPeriodicAlarm(alarm, 0, time, 0, callback);
}

BOOL
OSSetPeriodicAlarm(OSAlarm *alarm, uint32_t, OSTime start, OSTime interval, AlarmCallback callback)
{
   ScopedSpinLock lock(gAlarmLock);

   // Set alarm
   alarm->nextFire = start;
   alarm->callback = callback;
   alarm->period = interval;
   alarm->context = nullptr;
   alarm->state = OSAlarmState::Set;

   // Erase from old alarm queue
   if (alarm->alarmQueue) {
      OSEraseFromQueue(static_cast<OSAlarmQueue*>(alarm->alarmQueue), alarm);
   }

   // Add to this core's alarm queue
   auto core = OSGetCoreId();
   auto queue = gAlarmQueue[core];
   alarm->alarmQueue = queue;
   OSAppendQueue(queue, alarm);

   // Set the interrupt timer in processor
   gProcessor.setInterruptTimer(core, OSTimeToChrono(alarm->nextFire));
   return TRUE;
}

void
OSSetAlarmTag(OSAlarm *alarm, uint32_t alarmTag)
{
   alarm->alarmTag = alarmTag;
}

void
OSSetAlarmUserData(OSAlarm *alarm, void *data)
{
   alarm->userData = data;
}

BOOL
OSWaitAlarm(OSAlarm *alarm)
{
   OSAcquireSpinLock(gAlarmLock);
   assert(alarm);
   assert(alarm->tag == OSAlarm::Tag);

   if (alarm->state != OSAlarmState::Set) {
      OSReleaseSpinLock(gAlarmLock);
      return FALSE;
   }

   OSLockScheduler();
   OSSleepThreadNoLock(&alarm->threadQueue);
   OSReleaseSpinLock(gAlarmLock);
   OSRescheduleNoLock();
   OSUnlockScheduler();
   return TRUE;
}

static bool
OSTriggerAlarmNoLock(OSAlarm *alarm, OSContext *context)
{
   alarm->context = context;

   if (alarm->callback && alarm->state != OSAlarmState::Cancelled) {
      alarm->callback(alarm, context);
   }

   OSWakeupThread(&alarm->threadQueue);

   if (alarm->period) {
      alarm->nextFire = OSGetSystemTime() + alarm->period;
      alarm->state = OSAlarmState::Set;
   } else {
      alarm->nextFire = 0;
      alarm->state = OSAlarmState::None;
      OSEraseFromQueue(static_cast<OSAlarmQueue*>(alarm->alarmQueue), alarm);
   }

   return alarm->nextFire == 0;
}

void
OSCheckAlarms(uint32_t core, OSContext *context)
{
   ScopedSpinLock lock(gAlarmLock);
   auto queue = gAlarmQueue[core];
   auto now = OSGetSystemTime();

   for (OSAlarm *alarm = queue->head; alarm; ) {
      auto next = alarm->link.next;

      if (alarm->nextFire <= now) {
         OSTriggerAlarmNoLock(alarm, context);

         if (alarm->nextFire) {
            gProcessor.setInterruptTimer(core, OSTimeToChrono(alarm->nextFire));
         }
      }

      alarm = next;
   }
}

void
CoreInit::registerAlarmFunctions()
{
   RegisterKernelFunction(OSCancelAlarm);
   RegisterKernelFunction(OSCancelAlarms);
   RegisterKernelFunction(OSCreateAlarm);
   RegisterKernelFunction(OSCreateAlarmEx);
   RegisterKernelFunction(OSGetAlarmUserData);
   RegisterKernelFunction(OSSetAlarm);
   RegisterKernelFunction(OSSetPeriodicAlarm);
   RegisterKernelFunction(OSSetAlarmTag);
   RegisterKernelFunction(OSSetAlarmUserData);
   RegisterKernelFunction(OSWaitAlarm);
}

void
CoreInit::initialiseAlarm()
{
   gAlarmLock = OSAllocFromSystem<OSSpinLock>();

   for (auto i = 0u; i < 3; ++i) {
      gAlarmQueue[i] = OSAllocFromSystem<OSAlarmQueue>();
      OSInitAlarmQueue(gAlarmQueue[i]);
   }
}
