

void stm_start_shared_lock(void);
void stm_stop_shared_lock(void);
void stm_stop_exclusive_lock(void);
void stm_start_exclusive_lock(void);
void _stm_start_safe_point(void);
void _stm_stop_safe_point(void);
void _stm_reset_shared_lock();


