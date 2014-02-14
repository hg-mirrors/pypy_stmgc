
#if defined(__i386__) || defined(__x86_64__)

# define HAVE_FULL_EXCHANGE_INSN
  static inline void spin_loop(void) { asm("pause" : : : "memory"); }

#else

# warn "Add a correct definition of spin_loop() for this platform?"
  static inline void spin_loop(void) { asm("" : : : "memory"); }

#endif
