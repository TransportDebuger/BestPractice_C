# Паттерн обработки ошибок: Struct Result / Tagged Union (Структура результата)

title: "Паттерн обработки ошибок: Struct Result / Tagged Union (Структура результата)"\
date: "2026-07-01"\
version: "v1.0"\
standards: "C11 (базовый); GCC/Clang extensions (warn_unused_result); C23 ([[nodiscard]])"\
prerequisites:

- Уверенное владение структурами, объединениями (`union`) и перечислениями в C.
- Понимание паттернов [Паттерн 01: Return-код + out-параметры](01_return_code_out_params.md) и [Паттерн 05: Булевый успех/неуспех](05_bool_success.md).
- Знакомство с концепциями владения памятью (ownership) и временем жизни объектов.
- Знакомство с [Каталогом паттернов](00_pattern_catalog.md).

objective:

- Описать паттерн Tagged Union (Result) как способ объединения статуса и значения в один тип.
- Показать преимущества `union` перед "плоскими" структурами (экономия памяти, семантика взаимного исключения).
- Сформулировать строгие инварианты детерминизма и владения (ownership).
- Определить нишу паттерна и его связь с полиморфными макросами.

theory:
  Паттерн Result (вдохновлённый `Option`/`Result` из функциональных языков и `std::expected` из C++) в C наиболее точно и безопасно реализуется через **Tagged Union** (маркированное объединение).
  Мы используем структуру-дискриминатор (тег), которая указывает, какой именно тип данных сейчас активен в объединении (`union`).
  В отличие от "плоской" структуры, где поля `value` и `err` существуют одновременно (тратя память и размывая семантику), `union` гарантирует, что в любой момент времени валидно только одно из полей. Это делает код более безопасным, эффективным и идеологически верным.

---

## 1. Описание и строгий контракт

### 1.1. Базовая форма (Tagged Union)

```c
#include <stdbool.h>

typedef enum {
    ERR_OK = 0,
    ERR_PARSE_INVALID,
    ERR_PARSE_OVERFLOW
} err_t;

// Канонический Tagged Union
typedef struct {
    bool is_ok;             // Дискриминатор (Тег)
    union {
        int value;          // Полезная нагрузка (если is_ok == true)
        err_t err;          // Код ошибки (если is_ok == false)
    } data;
} result_int_t;

// C23: [[nodiscard("Ignoring Result may lead to undefined behavior")]]
// GCC/Clang: __attribute__((warn_unused_result))
__attribute__((warn_unused_result))
result_int_t parse_int(const char *s);
```

**Жесткие контракты и инварианты:**

1. **Взаимное исключение (Mutual Exclusion).** Если `is_ok == true`, валидно только `data.value`. Если `is_ok == false`, валидно только `data.err`. Обращение к неактивному полю `union` — это нарушение контракта и логическая ошибка.
2. **Инвариант детерминизма.** При `is_ok == false` мы обязаны инициализировать `data.err` осмысленным кодом. Поле `data.value` в этот момент считается мусором (так как оно перекрыто `err` в `union`), и читать его нельзя.
3. **Принудительная проверка.** Функции, возвращающие `Result`, **обязаны** помечаться `[[nodiscard]]` или `warn_unused_result` (см. [Паттерн 17](17_compiler_enforced_return_checking.md)). Игнорирование такой структуры — это игнорирование судьбы ресурса или данных.

### 1.2. Почему Union, а не плоская struct?

- **Память:** `sizeof(result_int_t)` с `union` = 8 байт (4 байта `bool` + padding + 4 байта `union`). Плоская структура (`bool`, `err`, `value`) заняла бы 12 байт.
- **Семантика:** `union` явно говорит компилятору и читателю: "Здесь либо данные, либо ошибка, одновременно — никогда". Плоская структура позволяет ошибочно записать и то, и другое, что ведет к трудноуловимым багам.

---

## 2. Плюсы и минусы

### 2.1. Плюсы

- **Самодостаточность.** Один тип содержит и статус, и значение. Это удобно для передачи, логирования и возврата из вспомогательных функций.
- **Экономия памяти.** Благодаря `union` размер структуры определяется только самым большим полем.
- **Выразительность.** Сигнатура `result_ptr_t alloc(size_t)` честнее, чем `void* alloc(size_t)`, так как явно указывает на возможность сбоя и наличие кода ошибки.

### 2.2. Минусы и риски

- **Накладные расходы на копирование.** Возврат больших структур по значению может быть неэффективен в hot-path. (Решается хранением указателя внутри `union`, а не самой структуры).
- **Сложность ABI.** Для библиотек со строгим бинарным совместимостью (ABI) передача сложных структур может быть менее предсказуема, чем передача `int` и `void**` (как в [Паттерне 01](01_return_code_out_params.md)).
- **Бойлерплейт.** Необходимость описывать отдельную структуру (`result_int_t`, `result_ptr_t`) для каждого типа данных. (Решается через [Паттерн 16: _Generic](16_type_safty_tagged_union.md)).

---

## 3. Простой пример: Парсинг целого числа

```c
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>

typedef enum { ERR_OK = 0, ERR_PARSE_INVALID, ERR_PARSE_OVERFLOW } err_t;

typedef struct {
    bool is_ok;
    union { int value; err_t err; } data;
} result_int_t;

__attribute__((warn_unused_result))
result_int_t parse_int(const char *s) {
    // Инициализация "ошибкой" по умолчанию (безопасное состояние)
    result_int_t res = { .is_ok = false, .data.err = ERR_OK };

    if (!s || *s == '\0') {
        res.data.err = ERR_PARSE_INVALID;
        return res;
    }

    errno = 0;
    char *endptr = NULL;
    long v = strtol(s, &endptr, 10);

    if (*endptr != '\0') {
        res.data.err = ERR_PARSE_INVALID;
        return res;
    }
    if (errno == ERANGE || v < INT_MIN || v > INT_MAX) {
        res.data.err = ERR_PARSE_OVERFLOW;
        return res;
    }

    // Успех: переключаем тег и пишем в union
    res.is_ok = true;
    res.data.value = (int)v;
    return res;
}

// Использование:
void demo(void) {
    result_int_t r = parse_int("42");
    if (r.is_ok) {
        printf("Value: %d\n", r.data.value); // Доступ через .data
    } else {
        printf("Error: %d\n", r.data.err);
    }
}
```

---

## 4. Развёрнутый пример: Result для указателя и Владение (Ownership)

Когда `Result` содержит указатель на динамическую память, `union` становится критически важным для четкого разделения "успешный указатель" / "код ошибки".

```c
#include <stdlib.h>
#include <stdbool.h>

typedef enum { ERR_OK = 0, ERR_INVALID_ARG, ERR_NO_MEM } err_t;

typedef struct {
    bool is_ok;
    union {
        void *ptr;  // Владеющий указатель
        err_t err;
    } data;
} result_ptr_t;

__attribute__((warn_unused_result))
result_ptr_t alloc_buffer(size_t size) {
    // Детерминизм: при ошибке указатель не должен быть "мусором"
    result_ptr_t res = { .is_ok = false, .data.err = ERR_OK };

    if (size == 0) {
        res.data.err = ERR_INVALID_ARG;
        return res;
    }

    void *p = malloc(size);
    if (!p) {
        res.data.err = ERR_NO_MEM;
        return res; // ptr остался NULL (так как union перекрыт err), утечки нет
    }

    res.is_ok = true;
    res.data.ptr = p; // Передача владения через union
    return res;
}
```

**Инварианты владения (Ownership Rules):**

1. Если `is_ok == true`: `data.ptr` валиден, вызывающий код **принимает владение** и обязан вызвать `free(data.ptr)`.
2. Если `is_ok == false`: `data.ptr` не валиден (там лежит `err_t`), освобождать ничего не нужно.
3. Атрибут `[[nodiscard]]` здесь — это защита от утечек памяти. Если написать `alloc_buffer(1024);` и проигнорировать результат, указатель будет потерян.

---

## 5. Взаимодействие с другими паттернами

### 5.1. vs Паттерн 01 (`err_t` + out-параметры)

- **[Паттерн 01](01_return_code_out_params.md)** лучше, когда нужно вернуть несколько разнородных значений, сложную структуру, или когда функция выполняет тяжелую работу с ресурсами (где `out`-параметры удобнее для передачи владения).
- **Паттерн 04** лучше для скалярных типов, парсеров и функций-генераторов значений, где результат логично возвращать через `return`.

### 5.2. vs Паттерн 05 (`bool` + out-параметры)

- **[Паттерн 05](05_bool_success.md)** идеален для внутренних фильтров, где причина ошибки не важна.
- **Паттерн 04** обязателен, если вызывающему коду нужно знать *причину* сбоя (через `err_t`) или если мы возвращаем владеющий указатель.

### 5.3. Синергия с Паттерном 16 (`_Generic`)

Главный минус Паттерна 04 — необходимость писать `result_int_t`, `result_ptr_t` вручную. В современном C (C11) эту проблему решает **[Паттерн 16: Типобезопасные Tagged Unions через _Generic](16_type_safty_tagged_union.md)**.
Паттерн 04 задает *семантику и структуру* (дискриминатор + union), а Паттерн 16 предоставляет *эргономичный синтаксис* (макросы `RESULT_OK(val)`), позволяя писать типобезопасный функциональный код без бойлерплейта.

---

## 6. Практические рекомендации (Best Practices)

1. **Используйте `union`.** Всегда предпочитайте Tagged Union плоской структуре для паттерна Result. Это экономит память и улучшает семантику.
2. **Доступ через дискриминатор.** Никогда не обращайтесь к полю `union` без предварительной проверки `is_ok`.
3. **Принудительная проверка.** Всегда помечайте функции `[[nodiscard]]` ([Паттерн 17](17_compiler_enforced_return_checking.md)). Потеря `Result` с указателем внутри — это гарантированная утечка памяти.
4. **Избегайте больших структур в `union`.** Если полезная нагрузка велика (например, 100 байт), `union` все равно придется копировать при возврате. В таких случаях используйте `union { MyStruct *ptr; err_t err; }`, то есть храните внутри только указатель на аллоцированный объект.

---

## References

**Внутренние перекрестные ссылки (Смежные паттерны):**

- [00_pattern_catalog.md](00_pattern_catalog.md) — Общий каталог паттернов обработки ошибок.
- [01_return_code_out_params.md](01_return_code_out_params.md) — Паттерн 01: Return-код + out-параметры (альтернатива для сложных API).
- [05_bool_success.md](05_bool_success.md) — Паттерн 05: Простое булево «успех/неуспех» (упрощенная версия без детализации).
- [16_type_safty_tagged_union.md](16_type_safty_tagged_union.md) — Паттерн 16: Типобезопасные Tagged Unions через `_Generic` (полиморфные макросы для Result).
- [17_compiler_enforced_return_checking.md](17_compiler_enforced_return_checking.md) — Паттерн 17: Принудительная проверка возврата (атрибуты `[[nodiscard]]`).

**Внешние стандарты и гайдлайны:**

- **ISO/IEC 9899:2011 (C11 Standard):** Раздел 6.5.2.3 (Structure and union members).
- **ISO/IEC 9899:2024 (C23 Standard):** Раздел по атрибутам `[[nodiscard]]`.
- **SEI CERT C Coding Standard:** [EXP05-C](https://wiki.sei.cmu.edu/confluence/display/c/EXP05-C) (Do not write to an object that is being accessed using a different type).
- **Rust / Haskell Documentation:** Концепции `Result<T, E>` и `Either` — идеологический источник паттерна.

## Changelog

  v1.0 — initial.
