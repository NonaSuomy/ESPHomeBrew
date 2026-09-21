#pragma once
#include "runtime.h"

/* Single producer/single consumer. The producer owns a slot until publishing
 * ready; the consumer releases it only after the synchronous I2S call ends.
 * No allocation, libc FILE access, or core state is shared with this task. */
#define EMU_AUDIO_SLOTS 3
#define EMU_AUDIO_FRAMES 1152
static struct {
    int16_t *data;
    unsigned count[EMU_AUDIO_SLOTS], ready[EMU_AUDIO_SLOTS];
    unsigned head, stop, done;
    void *task;
} emu_audio;

static void emu_audio_task(void *unused)
{
    (void)unused;
    unsigned tail=0;
    for (;;) {
        if (__atomic_load_n(&emu_audio.ready[tail],__ATOMIC_ACQUIRE)) {
            _papp_svc->audio_submit(emu_audio.data+tail*EMU_AUDIO_FRAMES*2,emu_audio.count[tail]);
            __atomic_store_n(&emu_audio.ready[tail],0,__ATOMIC_RELEASE);
            tail=(tail+1)%EMU_AUDIO_SLOTS;
        } else if (__atomic_load_n(&emu_audio.stop,__ATOMIC_ACQUIRE)) {
            /* Publication may have raced the first ready load. The stop
             * acquire makes the producer's final publication visible. */
            if (!__atomic_load_n(&emu_audio.ready[tail],__ATOMIC_ACQUIRE)) break;
        }
        else _papp_svc->delay_ms(1);
    }
    __atomic_store_n(&emu_audio.done,1,__ATOMIC_RELEASE);
    /* Owner deletes this task before unmapping PAPP code. Never return into
     * an unspecified task trampoline or delete while holding an I2S lock. */
    for (;;) _papp_svc->delay_ms(100);
}

static void emu_audio_start(int rate)
{
    _papp_svc->audio_init(rate);
    emu_audio.data=calloc(EMU_AUDIO_SLOTS*EMU_AUDIO_FRAMES*2,sizeof(int16_t));
    if (!emu_audio.data) return; /* synchronous fallback */
    if (_papp_svc->task_create(emu_audio_task,"papp_audio",4096,NULL,6,&emu_audio.task,1)) {
        free(emu_audio.data); emu_audio.data=NULL; emu_audio.task=NULL;
    }
}

static void emu_audio_submit(int16_t *samples, unsigned count)
{
    if (!emu_audio.task) {
        _papp_svc->audio_submit(samples,count);
        return;
    }
    while (count) {
        unsigned chunk=count>EMU_AUDIO_FRAMES ? EMU_AUDIO_FRAMES:count;
        unsigned slot=emu_audio.head;
        while (__atomic_load_n(&emu_audio.ready[slot],__ATOMIC_ACQUIRE)) _papp_svc->delay_ms(1);
        memcpy(emu_audio.data+slot*EMU_AUDIO_FRAMES*2,samples,chunk*2*sizeof(int16_t));
        emu_audio.count[slot]=chunk;
        __atomic_store_n(&emu_audio.ready[slot],1,__ATOMIC_RELEASE);
        emu_audio.head=(slot+1)%EMU_AUDIO_SLOTS;
        samples+=chunk*2; count-=chunk;
    }
}

static void emu_audio_finish(void)
{
    if (!emu_audio.task) return;
    __atomic_store_n(&emu_audio.stop,1,__ATOMIC_RELEASE);
    while (!__atomic_load_n(&emu_audio.done,__ATOMIC_ACQUIRE)) _papp_svc->delay_ms(1);
    _papp_svc->task_delete(emu_audio.task);
    emu_audio.task=NULL;
    free(emu_audio.data); emu_audio.data=NULL;
}
