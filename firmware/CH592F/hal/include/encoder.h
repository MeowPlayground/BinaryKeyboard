#ifndef KBD_ENCODER_H_
#define KBD_ENCODER_H_

#include "key.h"

#ifdef __cplusplus
extern "C" {
#endif

void Encoder_Init(void);
uint8_t Encoder_GetEvent(key_event_t *evt);
uint8_t Encoder_HandlePortIrq(gpio_port_t port);
void Encoder_TimerTick1ms(void);
void Encoder_EnterSleep(void);
void Encoder_ExitSleep(void);

#ifdef __cplusplus
}
#endif

#endif /* KBD_ENCODER_H_ */
