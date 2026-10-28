#ifndef COMPILER_ATTRS_H
#define COMPILER_ATTRS_H

/*
 * ============================================================================
 * КОМПИЛЯТОР-СПЕЦИФИЧНЫЕ АТРИБУТЫ ПРИНУДИТЕЛЬНОЙ ПРОВЕРКИ ВОЗВРАТА
 * ============================================================================
 */

/* Условное включение SAL-заголовка только для нативного MSVC */
#if defined(_MSC_VER) && !defined(__clang__)
    #include <sal.h>
#endif

/* ============================================================================
 * Макросы принудительной проверки возврата
 * ============================================================================ */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
    /* C23 и новее: стандартный атрибут ISO C */
    #define NODISCARD [[nodiscard]]
    
    /*
     * ВАЖНО: В стандарте C23 атрибут [[nodiscard]] НЕ принимает аргументов.
     * Только Clang поддерживает [[nodiscard("message")]] как расширение.
     * GCC в режиме -std=c23 выдаст ошибку компиляции при передаче строки.
     */
    #if defined(__clang__)
        #define NODISCARD_MSG(msg) [[nodiscard(msg)]]
    #else
        #define NODISCARD_MSG(msg) [[nodiscard]]
    #endif
    
#elif defined(__clang__) || defined(__GNUC__)
    /* Clang (включая clang-cl) или GCC в режимах до C23 */
    #define NODISCARD __attribute__((warn_unused_result))
    #define NODISCARD_MSG(msg) __attribute__((warn_unused_result))
    
#elif defined(_MSC_VER)
    /*
     * Нативный Microsoft Visual C++ (cl.exe) в режимах до C23
     * Ограничение: SAL работает только для функций, не для типов.
     */
    #define NODISCARD _Check_return_
    #define NODISCARD_MSG(msg) _Check_return_
    
#else
    /* Fallback для экзотических или устаревших компиляторов */
    #define NODISCARD
    #define NODISCARD_MSG(msg)
#endif

/* ============================================================================
 * МАКРОС ЯВНОГО ИГНОРИРОВАНИЯ РЕЗУЛЬТАТА
 * ============================================================================
 * Безопасно подавляет предупреждения nodiscard, гарантируя однократное
 * вычисление выражения и создание типизированной временной переменной.
 *
 * Использование:
 *   IGNORE_RESULT(fclose(f));
 *   IGNORE_RESULT(lib_cleanup(ctx));
 */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
    /* C23: стандартный typeof */
    #define IGNORE_RESULT(expr) \
        do { typeof(expr) _ignored_res_ = (expr); (void)_ignored_res_; } while(0)
#elif defined(__GNUC__) || defined(__clang__)
    /* GCC/Clang до C23: расширение __typeof__ */
    #define IGNORE_RESULT(expr) \
        do { __typeof__(expr) _ignored_res_ = (expr); (void)_ignored_res_; } while(0)
#else
    /*
     * Fallback для MSVC, экзотических или устаревших компиляторов
     * (Borland C, старые версии Tiny C, IAR/Keil без C99, и т.д.)
     *
     * ОГРАНИЧЕНИЕ: В этой ветке используется простой (void)-каст,
     * поскольку typeof недоступен. Это означает:
     *   - Для вызовов функций макрос работает корректно.
     *   - Для nodiscard-типов (структур с [[nodiscard]]) защита
     *     от предупреждений НЕ гарантируется на некоторых компиляторах.
     *   - Выражение вычисляется ровно один раз, побочные эффекты сохранены.
     */
    #define IGNORE_RESULT(expr) \
        do { (void)(expr); } while(0)
#endif

#endif /* COMPILER_ATTRS_H */