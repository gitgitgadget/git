#ifndef BANNED_H
#define BANNED_H

/*
 * This header lists functions that have been banned from our code base,
 * because they're too easy to misuse (and even if used correctly,
 * complicate audits). Including this header turns them into compile-time
 * errors.
 */

#define BANNED(func) sorry_##func##_is_a_banned_function
#define BANNED_EXPL(func, expl) sorry_##func##_is_a_banned_funcion_because_##expl##.

#undef strcpy
#define strcpy(x,y) BANNED_EXPL(strcpy, buffer_overflow_risk)
#undef strcat
#define strcat(x,y) BANNED(strcat)
#undef strncpy
#define strncpy(x,y,n) BANNED(strncpy)
#undef strncat
#define strncat(x,y,n) BANNED(strncat)
#undef strtok
#define strtok(x,y) BANNED(strtok)
#undef strtok_r
#define strtok_r(x,y,z) BANNED(strtok_r)

#undef sprintf
#undef vsprintf
#define sprintf(...) BANNED(sprintf)
#define vsprintf(...) BANNED(vsprintf)

#undef gmtime
#define gmtime(t) BANNED(gmtime)
#undef localtime
#define localtime(t) BANNED(localtime)
#undef ctime
#define ctime(t) BANNED(ctime)
#undef ctime_r
#define ctime_r(t, buf) BANNED(ctime_r)
#undef asctime
#define asctime(t) BANNED(asctime)
#undef asctime_r
#define asctime_r(t, buf) BANNED(asctime_r)

#endif /* BANNED_H */
