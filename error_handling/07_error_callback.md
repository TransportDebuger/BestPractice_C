# Паттерн обработки ошибок: callback-обработчики ошибок (Error Hooks)

title: "Паттерн обработки ошибок: callback-обработчики ошибок (Error Hooks)"\
date: "2026-07-01"\
version: "v1.0"\
standards: "C11 (базовый); GCC/Clang extensions (warn_unused_result); C23 ([[nodiscard]])"\
prerequisites:

- Базовое владение C (функции, указатели на функции, структуры).
- Понимание паттернов [Паттерн 01: Return-код + out-параметры](01_return_code_out_params.md) и [Паттерн 06: Контекст ошибок](06_error_context.md).
- Знакомство с многопоточностью и концепцией реентерабельности.
- Знакомство с [Каталогом паттернов](00_pattern_catalog.md).

objective:

- Описать паттерн callback-обработчиков (hooks) как механизм делегирования политики уведомления об ошибках.
- Жестко разграничить понятия «уведомление» (callback) и «управление потоком» (return-коды).
- Разобрать критические риски: реентерабельность, deadlocks и thread-safety.

theory:
  Паттерн callback-обработчиков предполагает, что библиотека при возникновении ошибок вызывает функцию, зарегистрированную пользователем, передавая ей код и контекст ошибки.\
  Ключевая философская установка: **Callback — это односторонний канал уведомления (hook), а не механизм управления потоком**. Библиотека не должна ждать от callback команд «повторить» или «прервать» — это разрушает синхронную модель C и ведет к катастрофическим багам. Управление потоком остается за `err_t` ([Паттерн 01](01_return_code_out_params.md)), а callback служит для логирования, метрик или UI.\
  Такой подход разделяет место обнаружения ошибки (библиотека) и место её реакции (приложение).

---

## 1. Описание и условия применения

### 1.1. Формальное описание и контракт

Callback-обработчик — это функция, регистрируемая в контексте библиотеки или глобально, которая вызывается при наступлении определенных событий (ошибок).

```c
typedef enum {
    ERR_OK = 0,
    ERR_IO,
    ERR_TIMEOUT,
    ERR_INTERNAL
} err_t;

// Callback строго для уведомления. Возвращает void.
typedef void (*error_hook_t)(err_t code, const char *message, void *user_data);

typedef struct lib_ctx lib_ctx_t;

void lib_set_error_hook(lib_ctx_t *ctx, error_hook_t hook, void *user_data);
```

**Жесткий контракт:**

1. **Только уведомление.** Callback вызывается исключительно для информирования. Он не должен возвращать код действия (retry/abort).
2. **Время жизни строки.** Библиотека гарантирует, что строка `message` валидна только **внутри** вызова callback. Если пользователю нужно сохранить её, он обязан сделать копию.
3. **Неотмена статуса.** Вызов callback не отменяет необходимости функции вернуть `err_t` вызывающему коду.

### 1.2. Когда паттерн уместен

- **Централизованное логирование/мониторинг:** Приложение хочет писать все ошибки библиотеки в свой лог-файл или отправлять в Sentry/Prometheus, не оборачивая каждый вызов.
- **Асинхронные события:** Ошибки возникают в фоновых потоках (worker pool, event loop), где нет прямого вызывающего стека для возврата `err_t`.
- **UI/Диагностика:** Глобальный перехватчик для показа диалоговых окон или обновления статус-баров.

---

## 2. Плюсы и минусы

### 2.1. Плюсы

- **Разделение ответственности.** Библиотека не знает о существовании `printf`, `syslog` или GUI. Приложение само решает, куда девать информацию.
- **Чистота основного API.** Не нужно заставлять пользователя передавать `log_ctx` в каждую функцию.
- **Идеально для асинхронности.** Единственный безопасный способ сообщить приложению об ошибке, упавшей в глубине worker-потока.

### 2.2. Минусы и риски

- **Скрытый поток управления.** Отладка усложняется: ошибка может быть залогирована в callback, но вызывающий код проигнорировал `err_t`, и состояние рассинхронизировалось.
- **Риск Deadlock и Reentrancy.** (См. раздел 5).
- **Производительность.** Вызов указателя на функцию (indirect call) плохо предсказывается процессором и может быть медленнее прямой проверки.

---

## 3. Простой пример: Перехватчик для логирования

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { ERR_OK = 0, ERR_IO, ERR_NO_MEM } err_t;
typedef void (*error_hook_t)(err_t code, const char *msg, void *ud);

typedef struct {
    error_hook_t hook;
    void *ud;
    // ... другое состояние ...
} lib_ctx_t;

void lib_set_error_hook(lib_ctx_t *ctx, error_hook_t hook, void *ud) {
    if (!ctx) return;
    ctx->hook = hook;
    ctx->ud = ud;
}

// Внутренний хелпер библиотеки
static void lib_report(lib_ctx_t *ctx, err_t code, const char *msg) {
    if (ctx && ctx->hook) {
        ctx->hook(code, msg, ctx->ud);
    }
}

__attribute__((warn_unused_result))
err_t lib_do_work(lib_ctx_t *ctx, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        lib_report(ctx, ERR_IO, "Failed to open file");
        return ERR_IO;
    }
    // ... работа ...
    fclose(f);
    return ERR_OK;
}

// --- Код приложения ---
void my_app_logger(err_t code, const char *msg, void *ud) {
    // ud может содержать указатель на файловый дескриптор лога
    fprintf(stderr, "[APP LOG] Code: %d, Msg: %s\n", code, msg);
}

int main(void) {
    lib_ctx_t ctx = {0};
    lib_set_error_hook(&ctx, my_app_logger, NULL);
    
    lib_do_work(&ctx, "nonexistent.txt"); 
    // Выведет: [APP LOG] Code: 1, Msg: Failed to open file
    return 0;
}
```

---

## 4. Развёрнутый пример: Контекст ошибок + Callback

В промышленных библиотеках callback комбинируется с контекстным объектом ошибки ([Паттерн 06](06_error_context.md)). Callback срабатывает «на лету», а контекст хранит состояние для синхронного чтения.

```c
typedef struct {
    err_t code;
    char message[128]; // Буфер фиксированного размера (безопасно, без malloc)
} error_state_t;

struct lib_ctx {
    error_state_t err;
    error_hook_t hook;
    void *hook_ud;
};

static void lib_clear_error(lib_ctx_t *ctx) {
    if (!ctx) return;
    ctx->err.code = ERR_OK;
    ctx->err.message[0] = '\0';
}

// Безопасная установка ошибки с вызовом hook
static void lib_set_error(lib_ctx_t *ctx, err_t code, const char *msg) {
    if (!ctx) return;
    
    ctx->err.code = code;
    // snprintf безопаснее strncpy: всегда ставит '\0' и не читает за пределами msg
    snprintf(ctx->err.message, sizeof(ctx->err.message), "%s", msg ? msg : "Unknown");
    
    if (ctx->hook) {
        ctx->hook(code, ctx->err.message, ctx->hook_ud);
    }
}

__attribute__((warn_unused_result))
err_t lib_connect(lib_ctx_t *ctx, const char *endpoint) {
    if (!ctx || !endpoint) return ERR_INVALID_ARG;
    lib_clear_error(ctx);

    /* ... попытка подключения ... */
    
    /* if (timeout) */ {
        lib_set_error(ctx, ERR_TIMEOUT, "Connection timed out");
        return ERR_TIMEOUT;
    }
    return ERR_OK;
}
```

---

## 5. Критические риски и правила безопасности (Must Read)

### 5.1. Реентерабельность и Deadlocks

**Золотое правило:** Callback **никогда не должен вызывать функции той же библиотеки**, если это явно не разрешено документацией.

- *Сценарий катастрофы:* `lib_connect` вызывает `hook`. Внутри `hook` пользователь решает переподключиться и вызывает `lib_connect`. Если библиотека использует внутренние мьютексы — произойдет **deadlock**. Если не использует — произойдет **переполнение стека** или затирание `error_state_t`.
- *Решение:* Документируйте это ограничение. Если библиотека должна быть реентерабельной, используйте thread-local storage для `error_state_t` или требуйте явного создания вложенных контекстов.

### 5.2. Проблема контекста потока (Thread Context)

Если библиотека многопоточна (например, сетевой фреймворк с worker-пулом), callback может быть вызван **из фонового потока**.

- *Сценарий катастрофы:* Пользователь внутри callback вызывает `printf` (в некоторых embedded-средах не thread-safe) или пытается обновить GUI (Windows API требует вызова из главного потока) -> **Crash**.
- *Решение:* Библиотека обязана документировать, из какого потока вызывается hook. Если потоки разные, пользователь должен не обрабатывать ошибку напрямую, а класть её в lock-free очередь (event queue) для главного потока.

### 5.3. Время жизни строки `message`

Строка, переданная в callback, часто указывает на внутренний буфер библиотеки (`ctx->err.message`).

- *Правило:* Пользователь **не имеет права** сохранять указатель `message` для использования после выхода из callback. Если нужно сохранить — делайте `strdup` или копируйте в свой буфер.

---

## 6. Практические рекомендации (Best Practices)

1. **Только уведомление (`void`).** Никогда не проектируйте callback, возвращающие `int` или `enum` для управления потоком (Retry/Abort). Это ломает предсказуемость C. Для Retry используйте циклы в вызывающем коде на основе `err_t`.
2. **Принудительная проверка.** Помечайте основные функции библиотеки `__attribute__((warn_unused_result))` ([Паттерн 17](17_compiler_enforced_return_checking.md)). Callback не отменяет необходимости проверять `err_t`.
3. **Безопасные буферы.** Если храните сообщение об ошибке в контексте, используйте `char buf[N]` и `snprintf`. Избегайте `malloc` внутри обработчика ошибок (если `malloc` упал с OOM, обработчик ошибки тоже упадет).
4. **Инициализация по умолчанию.** Если callback не установлен, библиотека должна работать абсолютно нормально (проверка `if (ctx->hook)` перед вызовом).

---

### References

**Внутренние перекрестные ссылки (Смежные паттерны):**

- [00_pattern_catalog.md](00_pattern_catalog.md) — Общий каталог паттернов обработки ошибок.
- [01_return_code_out_params.md](01_return_code_out_params.md) — Паттерн 01: Return-код + out-параметры (основной канал управления потоком, который callback не отменяет).
- [06_error_context.md](06_error_context.md) — Паттерн 06: Контекст ошибок (хранение состояния, которое дублируется в callback для синхронного чтения).
- [17_compiler_enforced_return_checking.md](17_compiler_enforced_return_checking.md) — Паттерн 17: Принудительная проверка возврата (атрибуты `[[nodiscard]]` для основных функций).

**Внешние стандарты и гайдлайны:**

- **SEI CERT C Coding Standard:**
  - [CON36-C](https://wiki.sei.cmu.edu/confluence/display/c/CON36-C) (Wrap functions that can spuriously wake up / reentrancy concepts).
  - [SIG30-C](https://wiki.sei.cmu.edu/confluence/display/c/SIG30-C) (Reentrancy considerations).
- **Linux Kernel Coding Style:** Раздел о том, почему явные return-коды предпочтительнее скрытых callback-ов для управления потоком.
- **ISO/IEC 9899:2024 (C23 Standard):** Раздел по атрибутам `[[nodiscard]]`.
- **libcurl Documentation:** Пример идеального использования callback-ов для tracing/debugging (`CURLOPT_DEBUGFUNCTION`) без вмешательства в flow-control.

## Changelog

  v1.0 — initial.
