# Паттерн обработки ошибок: Return-код (enum/int) + out-параметры

title: "Паттерн обработки ошибок: Return-код (enum/int) + out-параметры"\
date: "2026-07-01"\
version: "v1.0"\
standards: "C11 (базовый); GCC/Clang extensions (warn_unused_result); C23 ([[nodiscard]])"\
prerequisites:

- Базовое владение C (функции, указатели, перечисления, динамическая память).
- Понимание концепции return-значений, errno и out-параметров.
- Знакомство с [Каталогом паттернов](00_pattern_catalog.md).
- Знакомство с [Паттерном 05: Булевый успех/неуспех](05_bool_success.md).
  
objective:

- Подробно описать паттерн `err_t` + out-параметры как основной инструмент библиотечного уровня.
- Сформулировать строгие инварианты контракта для out-параметров и механизмы принуждения компилятора.
- Показать эволюцию кода: от простого примера к мини-библиотеке, и выявить потребность в централизованной очистке (goto).

theory:\
  Суть паттерна: функция возвращает статус (код ошибки) через `return`, а полезные данные — через параметры-указатели (out-параметры).\
  Код ошибки оформляется через перечисление (`enum`), что делает набор возможных ошибок явным, документируемым и расширяемым.\
  Паттерн хорошо подходит для библиотек и модулей среднего уровня, где важно отделить внутренние детали (например, POSIX-[errno](02_sentinel_errno.md), коды ОС) от «доменных» кодов ошибок.\
  В отличие от `errno`, этот паттерн не требует глобального состояния; он хорошо сочетается с тестированием и статическим анализом.

---

## 1. Описание и условия применения

### 1.1. Формальное описание и контракт

Форма сигнатуры:

```c
typedef enum {
    ERR_OK = 0,
    ERR_INVALID_ARG,
    ERR_IO,
    ERR_NO_MEM
} err_t;

// C23: [[nodiscard("Ignoring error code may lead to undefined behavior")]]
// GCC/Clang: __attribute__((warn_unused_result))
err_t func_name(arg1_t arg1, out_t *out_result);
```

**Жесткий контракт (Инварианты):**

1. При `ERR_OK` все out-параметры находятся в определённом, корректном состоянии.
2. При любом другом коде:
   - **Правило детерминизма:** Out-параметры **обязаны** быть приведены в безопасное состояние. Рекомендуется обнулять их в самом начале функции (через `memset` для структур или присваивание `NULL`/`0` для скаляров). Это гарантирует, что вызывающий код, случайно прочитавший данные до проверки `err_t`, не получит мусор из стека (предотвращение UB и утечек информации).
3. Вызвавший код обязан проверять `err_t` (компилятор должен принуждать к этому, см. [Паттерн 17](17_compiler_enforced_return_checking.md)).

### 1.2. Когда паттерн уместен

- Функция может завершиться с несколькими различными ошибками, и вызывающая сторона должна их различать.
- Результат функции логически «сложный» (структуры, несколько выходных значений), и использование sentinel-значений (`NULL`, `-1`) неудобно.
- Вы разрабатываете библиотеку, которая должна быть независима от глобального `errno`.

---

## 2. Плюсы и минусы

### 2.1. Плюсы

- **Явность контракта.** Наличие `err_t` в сигнатуре делает факт возможной ошибки очевидным.
- **Расширяемость.** Добавить новый тип ошибки можно, расширив `enum`, не меняя сигнатуры функций.
- **Независимость от errno.** Облегчает многопоточность и тестирование (можно мокать ошибки).

### 2.2. Минусы и риски

- **Громоздкость сигнатур.** При наличии нескольких выходных значений количество out-параметров растёт.
- **Риск игнорирования ошибок.** Статус можно проигнорировать.
  - *Решение:* Использование атрибутов `[[nodiscard]]` (C23) или `__attribute__((warn_unused_result))` (GCC/Clang). См. [Паттерн 17](17_compiler_enforced_return_checking.md).
- **Дублирование кода очистки.** Если функция захватывает несколько ресурсов, код освобождения при ошибках начинает дублироваться.
  - *Решение:* Применяется [Паттерн 10: Goto-цепочки для централизованной очистки](10_goto_cleanup.md).

---

### 3. Простой пример

### 3.1. Задача и код

```c
#include <stdio.h>
#include <stdlib.h>

typedef enum {
    ERR_OK = 0,
    ERR_INVALID_ARG,
    ERR_IO,
    ERR_PARSE
} err_t;

// Принудительная проверка возврата (Паттерн 17)
__attribute__((warn_unused_result))
err_t read_int(const char *path, int *out_value) {
    // 1. Детерминизм: обнуляем out-параметр сразу
    if (out_value) *out_value = 0;

    if (path == NULL || out_value == NULL) {
        return ERR_INVALID_ARG;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        return ERR_IO; // Здесь мы теряем детали errno, но сохраняем контракт
    }

    int tmp = 0;
    if (fscanf(f, "%d", &tmp) != 1) {
        fclose(f);
        return ERR_PARSE;
    }
    
    fclose(f);
    *out_value = tmp;
    return ERR_OK;
}
```

---

## 4. Развёрнутый пример: мини-библиотека конфигурации

### 4.1. Интерфейс

```c
typedef enum {
    ERR_OK = 0,
    ERR_CONFIG_INVALID_ARG,
    ERR_CONFIG_IO,
    ERR_CONFIG_PARSE,
    ERR_CONFIG_NO_MEM,
    ERR_CONFIG_NOT_FOUND
} err_t;

typedef struct config config_t;

__attribute__((warn_unused_result))
err_t config_load(const char *path, config_t **out_cfg);

void config_free(config_t *cfg);
```

### 4.2. Реализация (с безопасным аллокатором)

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct config_entry { char *key; char *value; };
struct config { struct config_entry *entries; size_t count; };

// Безопасный дубликатор строки (чистый C11, без утечек)
static char *safe_strdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char *dup = malloc(len);
    if (dup) memcpy(dup, s, len);
    return dup;
}

static err_t config_add_entry(config_t *cfg, const char *key, const char *value) {
    struct config_entry *new_entries = realloc(cfg->entries, (cfg->count + 1) * sizeof(*new_entries));
    if (!new_entries) return ERR_CONFIG_NO_MEM;
    cfg->entries = new_entries;

    // Безопасное выделение: если value упадет, мы освободим key
    char *k = safe_strdup(key);
    char *v = safe_strdup(value);
    if (!k || !v) {
        free(k); 
        free(v); 
        return ERR_CONFIG_NO_MEM;
    }

    cfg->entries[cfg->count].key = k;
    cfg->entries[cfg->count].value = v;
    cfg->count++;
    return ERR_OK;
}

err_t config_load(const char *path, config_t **out_cfg) {
    // 1. Детерминизм: обнуляем указатель
    if (out_cfg) *out_cfg = NULL; 
    
    if (!path || !out_cfg) return ERR_CONFIG_INVALID_ARG;

    FILE *f = fopen(path, "r");
    if (!f) return ERR_CONFIG_IO;

    config_t *cfg = calloc(1, sizeof(*cfg)); // calloc обнуляет поля
    if (!cfg) { fclose(f); return ERR_CONFIG_NO_MEM; }

    char line[256]; // Буфер достаточного размера
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) { fclose(f); config_free(cfg); return ERR_CONFIG_PARSE; }
        
        *eq = '\0';
        char *key = line;
        char *value = eq + 1;
        
        char *nl = strchr(value, '\n');
        if (nl) *nl = '\0';

        err_t err = config_add_entry(cfg, key, value);
        if (err != ERR_OK) {
            fclose(f); config_free(cfg); return err;
        }
    }
    fclose(f);
    
    *out_cfg = cfg;
    return ERR_OK;
}

void config_free(config_t *cfg) {
    if (!cfg) return;
    for (size_t i = 0; i < cfg->count; ++i) {
        free(cfg->entries[i].key);
        free(cfg->entries[i].value);
    }
    free(cfg->entries);
    free(cfg);
}
```

### 4.3. Анализ дизайна и «Мостик» к следующему паттерну

Внешний API полностью построен на `err_t`; `errno` нигде не «торчит» наружу. Низкоуровневые ошибки немедленно преобразуются в доменные коды.

**Внимание на код очистки:**
Посмотрите на функцию `config_load`. В ней **трижды** встречается связка:
`fclose(f); config_free(cfg); return err;`

Если бы у нас было 5 ресурсов (файл, 3 аллокации, мьютекс), код очистки превратился бы в «лапшу», где легко забыть освободить ресурс.
Именно для решения этой проблемы в C был канонизирован **[Паттерн 10: Goto-цепочки для централизованной очистки (Cleanup)](10_goto_cleanup.md)**, который позволяет избавиться от дублирования `free`/`close` и сделать поток освобождения ресурсов математически строгим.

---

## 5. Практические рекомендации (Best Practices)

1. **Принудительная проверка:** Всегда помечайте функции, возвращающие `err_t`, атрибутом `__attribute__((warn_unused_result))` или `[[nodiscard]]` ([Паттерн 17](17_compiler_enforced_return_checking.md)).
2. **Детерминизм out-параметров:** Обнуляйте out-параметры (`memset` или `= NULL`) в первой строке функции. Это защитит от UB и уязвимостей.
3. **Дизайн `err_t`:** Группируйте коды по доменам (`ERR_CONFIG_...`, `ERR_NET_...`). Резервируйте `0` для успеха. Имейте функцию `err_to_string()` для логирования.
4. **Маппинг `errno`:** Если вам нужна диагностика ошибок среды (например, отличить `ENOENT` от `EACCES`), перед возвратом `ERR_IO` логируйте значение `errno` ([Паттерн 02](02_sentinel_errno.md)) или сохраняйте его во внутреннем [контексте библиотеки](06_error_context.md).

---

## References

**Внутренние перекрестные ссылки (Смежные паттерны):**

- [00_pattern_catalog.md](00_pattern_catalog.md) — Общий каталог паттернов обработки ошибок.
- [02_sentinel_errno.md](02_sentinel_errno.md) — Паттерн 02: Специальное значение (sentinel) + errno (используется для маппинга низкоуровневых ошибок).
- [05_bool_success.md](05_bool_success.md) — Паттерн 05: Простое булево «успех/неуспех» (альтернатива для случаев, когда детализация не нужна).
- [06_error_context.md](06_error_context.md) — Паттерн 06: Контекст ошибок (используется для сохранения деталей `errno` и строк).
- [10_goto_cleanup.md](10_goto_cleanup.md) — Паттерн 10: Goto-цепочки для централизованной очистки (решает проблему дублирования кода освобождения ресурсов).
- [17_compiler_enforced_return_checking.md](17_compiler_enforced_return_checking.md) — Паттерн 17: Принудительная проверка возврата (атрибуты `[[nodiscard]]` и `warn_unused_result`).

**Внешние стандарты и гайдлайны:**

- **SEI CERT C Coding Standard:**
  - [EXP12-C](https://wiki.sei.cmu.edu/confluence/display/c/EXP12-C) (Do not ignore values returned by functions).
- **MISRA C:2012:**
  - Rule 17.7 (The value returned by a function having non-void return type shall be used).
- **ISO/IEC 9899:2024 (C23 Standard):** Раздел по атрибутам `[[nodiscard]]`.
- **Linux Kernel Coding Style:** Раздел 4 (Функции и goto) — обоснование того, почему `err_t` требует централизованной очистки.

## Changelog

  v1.0 — initial.
