# Паттерн обработки ошибок: latched error state (залипающее состояние ошибки)

title: "Паттерн обработки ошибок: latched error state (залипающее состояние ошибки)"\
date: "2026-07-01"\
version: "v1.0"\
standards: "C11 (базовый, _Atomic); GCC/Clang extensions (warn_unused_result); C23 ([[nodiscard]])"\
prerequisites:

- Понимание базовых паттернов обработки ошибок ([Паттерн 01: err_t](01_return_code_out_params.md), [Паттерн 02: errno](02_sentinel_errno.md), [Паттерн 06: Контекст ошибок](06_error_context.md)).
- Опыт работы с объектами/контекстами, имеющими жизненный цикл (init/use/destroy).
- Знакомство с многопоточностью и атомарными операциями C11.
- Знакомство с [Каталогом паттернов](00_pattern_catalog.md).

objective:

- Описать паттерн latched error state: перевод объекта/контекста в «залипающее» (перманентно ошибочное) состояние.
- Сформулировать строгий инвариант «однократной записи» (latch-once) для сохранения первопричины сбоя.
- Разобрать критические риски: состояния гонки (race conditions) и необходимость атомарности.
- Показать, как клиент извлекает root-cause из «мёртвого» объекта.

theory:

  В ряде систем после определённых ошибок (нарушения протокола, серьёзный I/O failure, внутренняя неконсистентность) объект или контекст практичнее считать неисправимым и запретить дальнейшие операции с ним. Это реализует принцип Circuit Breaker (предохранителя).\
  Паттерн latched error state — это надстройка над [Паттерном 06 (Контекст ошибок)](06_error_context.md). Мы берём `error_state_t` и добавляем к нему конечный автомат (Init -> Ready -> Failed).\
  Ключевой философский принцип: **объект, перешедший в состояние FAILED, больше не имеет права изменять свою диагностику**. Первая ошибка, убившая объект, должна быть сохранена навсегда, так как все последующие ошибки будут лишь её следствием.

---

## 1. Описание паттерна и строгий контракт

### 1.1. Формальное описание и контракт

```c
typedef enum {
    ERR_OK = 0,
    ERR_INVALID_ARG,
    ERR_IO,
    ERR_STREAM_FAILED
} err_t;

typedef struct {
    err_t code;
    char  message[128]; // Фиксированный буфер: защита от OOM при обработке ошибок
} error_state_t;

typedef struct {
    _Atomic int failed;      // C11: атомарный флаг состояния (0 - ок, 1 - мёртв)
    error_state_t err;       // Снимок первоначальной ошибки
    FILE *f;                 // Низкоуровневый ресурс
} safe_file_t;

// C23: [[nodiscard("Ignoring error code on a latched stream hides the root cause")]]
// GCC/Clang: __attribute__((warn_unused_result))
__attribute__((warn_unused_result))
err_t safe_file_write(safe_file_t *sf, const void *buf, size_t len);

const error_state_t *safe_file_last_error(const safe_file_t *sf);
```

**Жесткие контракты и инварианты:**

1. **Инвариант «Latch-Once» (Однократная запись).** Как только флаг `failed` переходит в `1`, поле `err` **никогда больше не перезаписывается**. Это гарантирует, что клиент всегда может прочитать первопричину (root cause) сбоя, а не сообщение вроде "object is dead", которое генерируется при последующих вызовах.
2. **Детерминизм сообщения.** Строковое сообщение всегда завершается `\0`. Использование `snprintf` вместо `strncpy` обязательно для предотвращения переполнения буфера.
3. **Атомарность перехода.** Проверка флага и его установка должны быть атомарными, чтобы избежать состояний гонки (см. раздел 5).

### 1.2. Когда паттерн уместен

- **Сетевые и I/O потоки:** после разрыва соединения или нарушения протокола дальнейшая работа с буферами бессмысленна и опасна.
- **Клиенты БД:** при получении фатальной ошибки от сервера (например, `SQLSTATE` класса `08` - Connection Exception) сессию нужно пересоздавать.
- **Драйверы и embedded:** при аппаратном сбое устройство переходит в состояние, требующее аппаратного или программного сброса (reset).

---

## 2. Плюсы и минусы

### 2.1. Плюсы

- **Защита от каскадных сбоев.** Предотвращает «работу с повреждённым состоянием» (use-after-fail), которая часто приводит к уязвимостям безопасности или тихой порче данных.
- **Упрощение логики клиента.** Клиенту не нужно анализировать каждую операцию. Если объект мёртв — все методы сразу возвращают `ERR_STREAM_FAILED`.
- **Сохранение диагностического контекста.** Благодаря инварианту latch-once, клиент всегда знает, *почему* объект умер, даже если с тех пор было сделано 100 вызовов.

### 2.2. Минусы и риски

- **Сложность классификации ошибок.** Нужно чётко решить, какие ошибки являются «восстановимыми» (например, `EAGAIN`), а какие «фатальными» (например, `EPIPE` или `ECONNRESET`).
- **Риск состояний гонки (Data Races).** В многопоточной среде наивная реализация флага `failed` приведёт к Undefined Behavior.

---

## 3. Простой пример: Безопасный файловый поток

### 3.1. Реализация с инвариантом Latch-Once

```c
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

// Вспомогательная функция установки фатальной ошибки
static void safe_file_set_error(safe_file_t *sf, err_t code, const char *msg) {
    if (!sf) return;
    
    // LATCH-ONCE RULE: 
    // Пытаемся атомарно изменить failed с 0 на 1.
    // Если оно уже было 1 (объект уже мёртв), мы просто выходим, 
    // сохраняя оригинальное сообщение об ошибке.
    int expected = 0;
    if (!atomic_compare_exchange_strong(&sf->failed, &expected, 1)) {
        return; 
    }
    
    sf->err.code = code;
    // snprintf безопасен: всегда ставит '\0' и не читает за пределами msg
    snprintf(sf->err.message, sizeof(sf->err.message), "%s", msg ? msg : "Unknown error");
}

__attribute__((warn_unused_result))
err_t safe_file_write(safe_file_t *sf, const void *buf, size_t len) {
    if (!sf || !buf) return ERR_INVALID_ARG;

    // Быстрая проверка: если объект уже мёртв, сразу возвращаем специфичный код
    if (atomic_load(&sf->failed)) {
        return ERR_STREAM_FAILED;
    }

    size_t n = fwrite(buf, 1, len, sf->f);
    if (n != len) {
        // Фатальная ошибка I/O. Переводим объект в latched state.
        safe_file_set_error(sf, ERR_IO, "fwrite failed: disk full or broken pipe");
        return ERR_IO; // При первом сбое возвращаем реальную причину
    }

    return ERR_OK;
}

const error_state_t *safe_file_last_error(const safe_file_t *sf) {
    if (!sf) return NULL;
    return &sf->err;
}
```

---

## 4. Взаимодействие с клиентским кодом (Чтение Root Cause)

Главная ценность паттерна раскрывается в том, как клиент обрабатывает «мёртвый» объект.

```c
int main(void) {
    safe_file_t sf = { .f = fopen("out.bin", "wb"), .failed = 0 };
    if (!sf.f) return 1;

    char data[] = "test data";
    
    // Предположим, что на диске закончилось место.
    err_t err = safe_file_write(&sf, data, sizeof(data));
    
    if (err != ERR_OK) {
        if (err == ERR_STREAM_FAILED) {
            // Объект уже был мёртв до этого вызова.
            // Читаем первоначальную причину из latched state!
            const error_state_t *root_cause = safe_file_last_error(&sf);
            fprintf(stderr, "Stream is dead. Root cause: [%d] %s\n", 
                    root_cause->code, root_cause->message);
        } else {
            // Это был ПЕРВЫЙ сбой. Объект только что перешёл в FAILED.
            // Сообщение уже сохранено внутри sf.err для будущих вызовов.
            const error_state_t *root_cause = safe_file_last_error(&sf);
            fprintf(stderr, "Stream just died. Reason: [%d] %s\n", 
                    root_cause->code, root_cause->message);
        }
        
        // В обоих случаях объект нужно закрыть/пересоздать
        fclose(sf.f);
        return 1;
    }
    
    fclose(sf.f);
    return 0;
}
```

---

## 5. Критические риски: Многопоточность и Атомарность

### 5.1. Состояние гонки (Race Condition)

Если `safe_file_t` используется из двух потоков, и Поток А проверяет `if (sf->failed)`, а Поток Б в этот же момент устанавливает `sf->failed = 1`, в C11 это классифицируется как **Data Race**, что является **Undefined Behavior (UB)**.

**Решения:**

1. **Использование `_Atomic` (C11).** Как показано в примере выше, флаг `failed` должен быть объявлен как `_Atomic int`, а операции чтения/записи должны использовать `atomic_load` и `atomic_compare_exchange_strong`. Это гарантирует корректность без тяжелых мьютексов.
2. **Документирование однопоточности.** Если использование атомарных операций нежелательно (например, в жестком embedded без поддержки C11 atomics), вы **обязаны** явно задокументировать: *"Объект не является потокобезопасным. Каждый поток должен использовать свой экземпляр safe_file_t"*.

### 5.2. Фиксированный буфер vs Динамическая память

Почему мы используем `char message[128]`, а не `char *message` с `malloc`?

- **Причина:** Обработчик ошибок **никогда не должен падать с OOM**. Если система исчерпала память (что и вызвало ошибку `ERR_NO_MEM` или `ERR_IO`), вызов `malloc` внутри `set_error` вернет `NULL`, и мы потеряем информацию о первопричине сбоя. Фиксированный буфер решает эту проблему.

---

## 6. Практические рекомендации (Best Practices)

1. **Принудительная проверка.** Помечайте методы объекта атрибутом `__attribute__((warn_unused_result))` или `[[nodiscard]]` ([Паттерн 17](17_compiler_enforced_return_checking.md)). Игнорирование кода возврата на "мёртвом" объекте ведет к тихой деградации.
2. **Соблюдайте Latch-Once.** Никогда не перезаписывайте `err.code` и `err.message`, если объект уже перешёл в состояние `FAILED`. Первая ошибка — это диагноз, последующие — лишь симптомы.
3. **Разделяйте Recoverable и Fatal ошибки.** Не делайте объект "мёртвым" из-за `EINTR` или `EAGAIN`. Latched state применяется только к фатальным сбоям (потеря связи, нарушение протокола, повреждение структур).
4. **Атомарность или Документация.** Либо используйте `_Atomic` для флага состояния, либо жестко документируйте ограничения многопоточности.
5. **Явный метод сброса (опционально).** Если объект поддерживает восстановление, предоставьте метод `safe_file_reset()`, который атомарно сбрасывает `failed` в `0` и очищает `err`, но только если базовый ресурс (например, `FILE*`) был успешно переподключен.

---

### References

**Внутренние перекрестные ссылки (Смежные паттерны):**

- [00_pattern_catalog.md](00_pattern_catalog.md) — Общий каталог паттернов обработки ошибок.
- [01_return_code_out_params.md](01_return_code_out_params.md) — Паттерн 01: Return-код + out-параметры (базовый механизм возврата статуса).
- [02_sentinel_errno.md](02_sentinel_errno.md) — Паттерн 02: Специальное значение + errno (источник фатальных ошибок среды, таких как `EPIPE`).
- [06_error_context.md](06_error_context.md) — Паттерн 06: Контекст ошибок (база, на которую накладывается конечный автомат latched state).
- [17_compiler_enforced_return_checking.md](17_compiler_enforced_return_checking.md) — Паттерн 17: Принудительная проверка возврата (атрибуты `[[nodiscard]]`).

**Внешние стандарты и гайдлайны:**

- **SEI CERT C Coding Standard:**
  - [CON36-C](https://wiki.sei.cmu.edu/confluence/display/c/CON36-C) (Wrap functions that can spuriously wake up / thread-safety concepts).
  - [EXP33-C](https://wiki.sei.cmu.edu/confluence/display/c/EXP33-C) (Do not read uninitialized memory / race conditions).
- **ISO/IEC 9899:2011 (C11 Standard):** Раздел 7.17 (Atomics) — семантика `_Atomic` и `atomic_compare_exchange`.
- **ISO/IEC 9899:2024 (C23 Standard):** Раздел по атрибутам `[[nodiscard]]`.
- **Martin Fowler, "Circuit Breaker":** Архитектурный паттерн, лежащий в основе концепции latched error state.
- **libcurl Documentation:** Пример использования `CURLOPT_FAILONERROR` и концепции "dead connection", требующей пересоздания handle.

## Changelog

  v1.0 — initial.
