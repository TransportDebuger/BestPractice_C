# Паттерн обработки ошибок: Типобезопасные Tagged Unions через _Generic

title: "Паттерн обработки ошибок: Типобезопасные Tagged Unions через _Generic"\
date: "2026-07-01"\
version: "v1.0"\
standards: "C11 (ключевая фича: 6.5.1.1 Generic selection); GCC/Clang extensions (__auto_type)"\
prerequisites:

- Глубокое понимание [Паттерна 04: Struct Result / Tagged Union](04_result_struct.md).
- Уверенное владение макросами C и концепциями статической типизации.
- Знакомство с [Каталогом паттернов](00_pattern_catalog.md).

objective:

- Описать использование `_Generic` для создания полиморфных, типобезопасных конструкторов Result-типов.
- Показать, как устранить бойлерплейт при работе с Tagged Unions для разных типов данных.
- Продемонстрировать синергию `_Generic` с расширением `__auto_type` для локального вывода типов.
- Определить границы применимости и влияние на сообщения компилятора.

theory:
  [Паттерн 04](04_result_struct.md) (Tagged Union) требует создания отдельной структуры и конструктора для каждого возвращаемого типа (`result_int_t`, `result_ptr_t`). Это утомительно и засоряет код.\
  Ключевое слово `_Generic`, введенное в стандарте C11, позволяет выбирать выражение на этапе компиляции в зависимости от типа операнда.\
  Ключевой философский принцип: **Полиморфизм без потери типобезопасности и без динамических аллокаций**. Мы используем `_Generic` для создания макросов `RESULT_OK(val)` и `RESULT_ERR(code, type)`, которые автоматически "упаковывают" значение в нужный `union` и возвращают структуру строго правильного типа. Компилятор гарантирует, что мы не сможем присвоить `result_ptr_t` переменной типа `result_int_t`.

---

## 1. Описание и строгий контракт

### 1.1. Базовая форма: Специализированные структуры

В отличие от подхода с "резиновым" `union` (который тратит память на максимальный тип), мы определяем легковесные структуры для каждого типа, а `_Generic` выступает в роли диспетчера.

```c
#include <stdbool.h>
#include <stddef.h>

typedef enum { ERR_OK = 0, ERR_NO_MEM, ERR_PARSE } err_t;

// 1. Базовые структуры (легковесные, специализированные)
typedef struct { bool is_ok; union { int val; err_t err; } data; } res_int_t;
typedef struct { bool is_ok; union { double val; err_t err; } data; } res_double_t;
typedef struct { bool is_ok; union { void *ptr; err_t err; } data; } res_ptr_t;

// 2. Inline-конструкторы (гарантия нулевых накладных расходов)
static inline res_int_t make_res_int(bool ok, err_t e, int v) {
    res_int_t r = { .is_ok = ok };
    if (ok) r.data.val = v; else r.data.err = e;
    return r;
}
static inline res_double_t make_res_double(bool ok, err_t e, double v) {
    res_double_t r = { .is_ok = ok };
    if (ok) r.data.val = v; else r.data.err = e;
    return r;
}
static inline res_ptr_t make_res_ptr(bool ok, err_t e, void *p) {
    res_ptr_t r = { .is_ok = ok };
    if (ok) r.data.ptr = p; else r.data.err = e;
    return r;
}
```

### 1.2. Полиморфные макросы (Диспетчеризация через `_Generic`)

Теперь мы создаем макросы, которые скрывают рутину.

```c
// Макрос успеха: тип определяется по типу аргумента `val`
#define RESULT_OK(val) _Generic((val), \
    int:    make_res_int, \
    double: make_res_double, \
    char*:  make_res_ptr, \
    void*:  make_res_ptr  \
)(true, ERR_OK, val)

// Макрос ошибки: тип указывается явно через второй аргумент (type hint)
// Мы используем указатель на тип (T*)0, чтобы _Generic мог сопоставить тип без создания значения
#define RESULT_ERR(code, T) _Generic((T*)0, \
    int*:    make_res_int(false, code, 0), \
    double*: make_res_double(false, code, 0.0), \
    char**:  make_res_ptr(false, code, NULL), \
    void**:  make_res_ptr(false, code, NULL) \
)
```

**Жесткие контракты и инварианты:**

1. **Строгая типизация на этапе компиляции.** `RESULT_OK(42)` вернет `res_int_t`. Попытка присвоить его в `res_ptr_t` вызовет ошибку компиляции (incompatible types).
2. **Исчерпывающий список типов.** Макрос `_Generic` должен покрывать все типы, используемые в проекте. Если тип не найден, компилятор выдаст ошибку (что правильно, так как предотвращает неявные приведения).
3. **Детерминизм при ошибке.** Конструкторы `make_res_*` гарантируют, что при `is_ok == false` поле `val`/`ptr` инициализируется безопасным значением (`0`, `0.0`, `NULL`), даже если оно находится в `union` и перекрыто полем `err`.

---

## 2. Области применения

Паттерн является стандартом для:

- **Современных C-библиотек (C11+):** Парсеры, конвертеры, математические вычисления, где функции логически возвращают одно значение, но могут упасть.
- **Замены небезопасных макросов:** Вместо макросов, возвращающих `void*` или использующих приведение типов, `_Generic` обеспечивает 100% типобезопасность.
- **Функционального стиля в C:** Позволяет писать цепочки вызовов (pipeline), где каждая функция возвращает `Result`, а распаковка происходит только в конце.

---

## 3. Плюсы и минусы

### 3.1. Плюсы

- **Отсутствие бойлерплейта.** Не нужно писать `result_int_t`, `result_double_t` вручную в каждом модуле. Достаточно один раз определить инфраструктуру.
- **Нулевой overhead.** `_Generic` и `static inline` полностью вычисляются на этапе компиляции. В бинарнике не будет никаких "диспетчеров" или `switch` по тегам.
- **Экономия памяти.** Каждая специализированная структура занимает ровно столько, сколько нужно для её типа (например, `res_int_t` = 8 байт, а не 16 или 24, как в универсальном `union`).
- **Безопасность.** Невозможно случайно извлечь `int` из `res_ptr_t` без явного приведения, так как это разные типы данных для компилятора.

### 3.2. Минусы и риски

- **Требует C11.** Не скомпилируется в старых компиляторах или в режиме strict C99.
- **Сложные сообщения об ошибках.** Если вы передадите в `RESULT_OK` неподдерживаемый тип (например, `long long`), сообщение компилятора о несовпадении типов в `_Generic` может быть трудным для чтения.
- **Необходимость явного указания типа для ошибок.** В макросе `RESULT_ERR(code, T)` приходится передавать "заглушку" типа, так как C не может вывести тип результата из контекста присваивания (в отличие от Rust).

---

## 4. Продвинутый уровень: Вывод типов через `__auto_type`

Главная проблема полиморфных макросов в C — отсутствие ключевого слова `auto` (как в C++11). Нам приходится явно писать тип переменной:

```c
res_int_t r1 = RESULT_OK(42); // Приходится знать тип заранее
```

**Решение (GCC/Clang Extension):** Использование `__auto_type`. Это расширение позволяет компилятору вывести тип переменной из типа инициализатора. В связке с `_Generic` это дает невероятно чистый код.

```c
// Использование __auto_type для локального вывода типов
void demo(void) {
    // Компилятор сам поймет, что r1 — это res_int_t
    __auto_type r1 = RESULT_OK(42); 
    
    // Компилятор сам поймет, что r2 — это res_ptr_t
    __auto_type r2 = RESULT_OK(malloc(1024)); 
    
    // Для явной ошибки мы передаем тип-заглушку
    __auto_type r3 = RESULT_ERR(ERR_NO_MEM, int); // r3 будет res_int_t
    
    if (r1.is_ok) {
        printf("Value: %d\n", r1.data.val);
    }
}
```

*Примечание:* `__auto_type` поддерживается GCC и Clang. В строгом ISO C11 его нет, но в современных user-space проектах (Linux, systemd) оно используется повсеместно.

---

## 5. Развёрнутый пример: Полиморфный парсер

Покажем, как это выглядит в реальном коде парсера, который может возвращать разные типы.

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ... (Инфраструктура res_int_t, res_ptr_t, RESULT_OK, RESULT_ERR из разделов 1.1 и 1.2) ...

// Функция, которая может вернуть либо число, либо строку
__attribute__((warn_unused_result))
res_int_t parse_config_count(const char *val) {
    if (!val) return RESULT_ERR(ERR_INVALID_ARG, int);
    
    char *endptr;
    long v = strtol(val, &endptr, 10);
    if (*endptr != '\0') return RESULT_ERR(ERR_PARSE, int);
    
    return RESULT_OK((int)v);
}

__attribute__((warn_unused_result))
res_ptr_t parse_config_name(const char *val) {
    if (!val) return RESULT_ERR(ERR_INVALID_ARG, char*);
    
    char *dup = strdup(val);
    if (!dup) return RESULT_ERR(ERR_NO_MEM, char*);
    
    return RESULT_OK(dup);
}

int main(void) {
    // Благодаря __auto_type код читается как на Python/Rust
    __auto_type count_res = parse_config_count("42");
    if (!count_res.is_ok) {
        fprintf(stderr, "Failed to parse count\n");
        return 1;
    }
    printf("Count: %d\n", count_res.data.val);

    __auto_type name_res = parse_config_name("my_app");
    if (!name_res.is_ok) {
        fprintf(stderr, "Failed to parse name\n");
        return 1;
    }
    printf("Name: %s\n", (char*)name_res.data.ptr);
    free(name_res.data.ptr); // Не забываем освободить память!

    return 0;
}
```

---

## 6. Взаимодействие с другими паттернами

### 6.1. Синергия с Паттерном 04 (Tagged Union)

[Паттерн 04](04_result_struct.md) — это не замена Паттерну 16, а его эволюция и надстройка. Паттерн 04 задает семантику (дискриминатор + union), а Паттерн 16 предоставляет эргономичный, типобезопасный синтаксис для работы с этой семантикой.

### 6.2. Синергия с Паттерном 17 (Compiler Enforcement)

Функции, возвращающие полиморфные `Result`, **обязаны** помечаться `[[nodiscard]]` или `warn_unused_result`. Поскольку `Result` может содержать владеющий указатель (как в `res_ptr_t`), игнорирование возврата приведет не просто к потере данных, а к утечке памяти. Компилятор должен жестко пресекать такие попытки (см. [Паттерн 17](17_compiler_enforced_return_checking.md)).

---

## 7. Практические рекомендации (Best Practices)

1. **Централизация инфраструктуры.** Определите базовые структуры (`res_int_t`, `res_ptr_t`) и макросы (`RESULT_OK`, `RESULT_ERR`) в одном общем заголовочном файле (например, `result_generic.h`). Не дублируйте их в каждом модуле.
2. **Обработка неподдерживаемых типов.** Если вы хотите жестко контролировать типы, добавьте ветку `default` в `_Generic`, которая вызывает заведомо несуществующую функцию или `static_assert`:

```c
   #define RESULT_OK(val) _Generic((val), \
       int: make_res_int, \
       /* ... */ \
       default: unsupported_result_type(val) \
   )(true, ERR_OK, val)
```

   Это заставит компилятор выдать ошибку, если разработчик попытается вернуть `long long` или кастомную структуру, не добавив её в список поддерживаемых.
3. **Избегайте сложных структур в `union`.** Если вам нужно вернуть большую структуру (например, `config_t`), не кладите её в `union` по значению. Используйте `res_ptr_t` и возвращайте указатель на аллоцированный объект. `union` должен содержать только скаляры и указатели.
4. **Документируйте поддерживаемые типы.** В комментарии к макросу `RESULT_OK` явно перечислите типы, которые он поддерживает (например, `@supports int, double, char*, void*`).

---

## References

**Внутренние перекрестные ссылки (Смежные паттерны):**

- [00_pattern_catalog.md](00_pattern_catalog.md) — Общий каталог паттернов обработки ошибок.
- [04_result_struct.md](04_result_struct.md) — Паттерн 04: Struct Result / Tagged Union (база, для которой `_Generic` предоставляет полиморфный синтаксис).
- [17_compiler_enforced_return_checking.md](17_compiler_enforced_return_checking.md) — Паттерн 17: Принудительная проверка возврата (атрибуты `[[nodiscard]]`, критичные для Result с указателями).

**Внешние стандарты и гайдлайны:**

- **ISO/IEC 9899:2011 (C11 Standard):** Раздел 6.5.1.1 (Generic selection) — официальная спецификация `_Generic`.
- **GCC Manual:** [Typeof and _Generic](https://gcc.gnu.org/onlinedocs/gcc/Typeof.html) — документация по `__auto_type` и `typeof`.
- **Rust Documentation:** Концепция `Result<T, E>` и type inference — идеологический источник паттерна.
- **SEI CERT C Coding Standard:** [EXP34-C](https://wiki.sei.cmu.edu/confluence/display/c/EXP34-C) (Do not dereference null pointers) — важно для `res_ptr_t` при ошибке.
- **Linux Kernel Coding Style:** Раздел о использовании макросов и inline-функций для обеспечения типобезопасности без накладных расходов.

## Changelog

  v1.0 — initial.
