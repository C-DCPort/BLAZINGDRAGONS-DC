/* Remplace le config.h produit par le configure de ScummVM. */
#ifndef BD_CONFIG_H
#define BD_CONFIG_H
#if defined(__DC__) || defined(_arch_dreamcast)
#define SCUMM_LITTLE_ENDIAN
#define SCUMM_NEED_ALIGNMENT
#else
#define SCUMM_LITTLE_ENDIAN
#endif
#define DISABLE_TEXT_CONSOLE_PRINTF 0
#endif
