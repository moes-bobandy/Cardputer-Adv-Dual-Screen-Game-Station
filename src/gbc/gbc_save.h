#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void gbc_save_init(const char* romPathOrName);
void gbc_save_load(void);
void gbc_save_tick(void);
void gbc_save_request_flush(void);
void gbc_save_force_flush(void);

#ifdef __cplusplus
}
#endif
