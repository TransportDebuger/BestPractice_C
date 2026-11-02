# Паттерн обработки ошибок: специальное значение (sentinel) + errno

title: "Паттерн обработки ошибок: специальное значение (sentinel) + errno"\
date: "2026-07-01"\
version: "v1.0"\
standards: "C11 (ISO C); POSIX.1-2017 (расширения errno); GCC/Clang extensions (warn_unused_result); C23 ([[nodiscard]])"\
prerequisites:

- Понимание стандартной библиотеки C (stdio, stdlib) и системных вызовов POSIX.
- Базовое знание концепции errno и sentinel-значений (NULL, -1, EOF).
- Знакомство с [Каталогом паттернов](00_pattern_catalog.md).
- Знакомство с [Паттерном 08: Маркеры состояния и конца потока](08_state_stream_marker.md) (для понимания жесткого разделения EOF и ошибок среды).

objective:

- Подробно описать паттерн «sentinel + errno» как стандарт взаимодействия с ОС и stdlib.
- Развенчать мифы о многопоточности errno и сформулировать реальную опасность «затирания» (clobbering).
- Показать строгие правила чтения errno, обнуления перед вызовом и разделения EOF/Ошибки.
- Определить границы применимости и моменты, когда необходимо переходить на err_t.

theory:\
  Паттерн «sentinel + errno» означает, что функция возвращает значение обычного типа, при этом одно значение из диапазона договорённо трактуется как признак ошибки (sentinel), а детали ошибки передаются через индикатор `errno`.\
  Важно строго разделять стандарты: **ISO C** гарантирует лишь макрос `errno` и три кода (`EDOM`, `ERANGE`, `EILSEQ`). Все остальные коды (`EINVAL`, `EINTR`, `ENOENT` и т.д.) — это **POSIX**. Написание кроссплатформенного кода (Windows, bare-metal) с опорой на POSIX-коды errno без условий компиляции сломает его.\
  Паттерн является de-facto стандартом для низкоуровневого взаимодействия с Unix-подобными системами, но требует высочайшей дисциплины при чтении и сохранении состояния `errno`.

---

## 1. Описание и строгий контракт

### 1.1. Формальное описание

```c
/* Пример для указателей */
T *func(...);   // при успехе — корректный указатель, при ошибке — NULL
/* Пример для целых */
int func(...);  // при успехе — >= 0, при ошибке — -1
```

При возврате sentinel-значения функция устанавливает `errno` в соответствующий код (например, `ENOENT`, `EACCES`, `ENOMEM`).

### 1.2. Золотые правила работы с errno

1. **Правило немедленного чтения.** `errno` нужно читать **сразу** после получения sentinel-значения. Любой промежуточный вызов функции (даже успешный `printf`, `malloc`, `fclose` или `strerror`) может перезаписать `errno` в `0` или другой код.
2. **Правило сохранения (Save & Restore).** Если перед обработкой ошибки необходимо вызвать другие функции, `errno` нужно сохранить в локальную переменную:

   ```c
   int fd = open(path, O_RDONLY);
   if (fd < 0) {
       const int saved_errno = errno; // Единственное корректное место для сохранения
       log_something();               // Эта функция может затереть errno
       return map_errno(saved_errno);
   }
   ```

3. **Правило предварительного обнуления (`errno = 0`).** Некоторые функции (например, `strtol`, `strtod`, `readdir`, `getpriority`) могут легально вернуть sentinel-значение (например, `0` или `NULL`) как при успехе, так и при ошибке. Для них вызывающий код **обязан** явно установить `errno = 0` *перед* вызовом.

### 1.3. Разделение EOF (конец данных) и Ошибки среды

В нашем каталоге мы жестко разделяем описываемый паттерн и [Паттерн 08: Маркеры состояния и конца потока](08_state_stream_marker.md). Функции вроде `fgetc` возвращают `EOF` в обоих случаях. Слепая проверка `if (c == EOF)` без уточнения причины — классическая ошибка.

- **Идиома разделения:**

    ```c
    int c = fgetc(f);
    if (c == EOF) {
        if (ferror(f)) {
            // Паттерн 02: Ошибка среды (сбой чтения, I/O error)
            handle_io_error(errno); 
        } else {
            // Паттерн 08: Естественный конец файла (норма)
            handle_eof();
        }
    }
    ```

---

## 2. Области применения

Паттерн уместен, когда:

- Вы пишете обёртку поверх стандартной библиотеки или POSIX-функций.
- Возвращаемый тип имеет «естественное» sentinel-значение (`NULL` для указателей, `-1` для дескрипторов).
- Сценарий подразумевает работу с конкретными ошибками ОС, и вызывающий код находится на том же уровне абстракции (системный код, драйверы, низкоуровневые утилиты).

---

## 3. Плюсы и минусы

### 3.1. Плюсы

- **Естественная интеграция с ОС.** Соответствует стилю 99% системных вызовов и функций stdlib.
- **Минимальный overhead.** Не нужно определять новые типы ошибок; достаточно sentinel и `errno`.
- **Привычность.** Для C/POSIX-разработчиков это стандартный и ожидаемый способ работы.

### 3.2. Минусы и реальные риски

- **Затирание (Clobbering).** Главная опасность `errno` — не многопоточность (в POSIX `errno` — это макрос, раскрывающийся в `_Thread_local` переменную, что решает проблему потоков), а затирание промежуточными вызовами.
- **Потокобезопасность `strerror`.** Стандартная функция `strerror(errno)` **не является потокобезопасной**, так как может возвращать указатель на статический внутренний буфер. В многопоточном коде необходимо использовать `strerror_r` (POSIX) или `strerror_l`.
- **Неконсистентность между платформами.** Набор кодов `errno` и поведение некоторых функций отличается между ОС. ISO C гарантирует только `EDOM`, `ERANGE`, `EILSEQ`.
- **Сложность тестирования.** Манипуляция глобальным (или TLS) `errno` затрудняет написание изолированных юнит-тестов.

---

## 4. Простой пример: Обёртка над открытием файла

```c
#include <stdio.h>
#include <errno.h>
#include <string.h>

// GCC/Clang: __attribute__((warn_unused_result))
// C23: [[nodiscard("Ignoring file pointer may lead to resource leaks")]]
__attribute__((warn_unused_result))
FILE *open_data_file(const char *path) {
    if (path == NULL) {
        errno = EINVAL;  // Явная установка при нарушении контракта API
        return NULL;
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        // errno уже установлен внутри fopen
        return NULL;
    }
    return f;
}

int main(void) {
    FILE *f = open_data_file("data.txt");
    if (!f) {
        // ПРАВИЛО 1: Читаем errno СРАЗУ.
        // Используем strerror_r для потокобезопасности (POSIX).
        char err_buf[256];
        // Примечание: strerror_r имеет два варианта (XSI и GNU), 
        // здесь показан концепт безопасного получения строки.
        strerror_r(errno, err_buf, sizeof(err_buf));
        
        fprintf(stderr, "open_data_file failed: %s\n", err_buf);
        return 1;
    }
    /* работа с файлом */
    fclose(f);
    return 0;
}
```

---

## 5. Развёрнутый пример: Обёртка над POSIX read и правило errno=0

### 5.1. Обработка EINTR и сохранение errno

```c
#include <unistd.h>
#include <errno.h>

__attribute__((warn_unused_result))
ssize_t read_full(int fd, void *buf, size_t count) {
    if (count == 0) return 0;
    if (buf == NULL) {
        errno = EINVAL;
        return -1;
    }

    size_t total = 0;
    char *p = buf;
    while (total < count) {
        ssize_t n = read(fd, p + total, count - total);
        if (n > 0) {
            total += (size_t)n;
        } else if (n == 0) {
            break; // EOF
        } else {
            // n < 0: ошибка
            if (errno == EINTR) {
                continue; // Прерван сигналом — повторяем (errno не трогаем)
            }
            // Прочие ошибки: возвращаем -1. 
            // Вызывающий код должен СРАЗУ прочитать errno.
            return -1;
        }
    }
    return (ssize_t)total;
}
```

### 5.2. Правило `errno = 0` на примере `strtol`

```c
#include <stdlib.h>
#include <errno.h>
#include <limits.h>

long safe_parse_long(const char *str, int *out_error) {
    // ПРАВИЛО 3: Обнуляем errno ПЕРЕД вызовом функции,
    // которая не очищает его при успехе.
    errno = 0; 
    
    char *endptr = NULL;
    long val = strtol(str, &endptr, 10);

    if (endptr == str) {
        *out_error = 1; // Не было цифр
        return 0;
    }
    if (errno == ERANGE) {
        *out_error = 2; // Переполнение
        return 0;
    }
    
    *out_error = 0;
    return val;
}
```

---

## 6. Взаимодействие с другими паттернами

### 6.1. Маппинг errno в err_t ([Паттерн 01](01_return_code_out_params.md))

На низком уровне используется `sentinel + errno`. На среднем уровне библиотеки эти ошибки немедленно маппятся в доменный `err_t`, чтобы скрыть детали ОС от клиента:

```c
err_t config_load(const char *path, config_t **out_cfg) {
    FILE *f = fopen(path, "r");
    if (!f) {
        // Маппинг POSIX-ошибок в доменные коды
        if (errno == ENOENT) return ERR_CONFIG_NOT_FOUND;
        if (errno == EACCES) return ERR_CONFIG_PERMISSION;
        return ERR_CONFIG_IO; // Остальные ошибки I/O
    }
    // ...
}
```

---

## 7. Практические рекомендации (Best Practices)

1. **Принудительная проверка.** Функции, возвращающие sentinel-значения (особенно целочисленные, где `-1` легко пропустить), должны помечаться атрибутом `__attribute__((warn_unused_result))` или `[[nodiscard]]` ([Паттерн 17](17_compiler_enforced_return_checking.md)).
2. **Никогда не используйте `strerror` в многопоточном коде.** Всегда используйте `strerror_r` или `strerror_l`, либо копируйте результат в локальный буфер немедленно.
3. **Разделяйте ISO C и POSIX.** Если ваш код должен компилироваться в Windows или freestanding-среде, не полагайтесь на `EINTR`, `ENOENT` и т.д. без макросов условной компиляции (`#ifdef _POSIX_C_SOURCE`).
4. **Сохраняйте errno.** Если между получением ошибки и её логированием/обработкой есть хоть один вызов другой функции, сохраняйте `errno` в `const int saved_errno`.
5. **Не используйте errno для логических ошибок.** `errno` предназначен только для ошибок среды и ОС. Для логических ошибок приложения используйте `err_t` или `Result`.

---

## References

**Внутренние перекрестные ссылки (Смежные паттерны):**

- [00_pattern_catalog.md](00_pattern_catalog.md) — Общий каталог паттернов обработки ошибок.
- [01_return_code_out_params.md](01_return_code_out_params.md) — Паттерн 01: Return-код + out-параметры (используется для маппинга errno в доменные коды).
- [08_state_stream_marker.md](08_state_stream_marker.md) — Паттерн 08: Маркеры состояния и конца потока (разделение EOF и ошибок среды).
- [17_compiler_enforced_return_checking.md](17_compiler_enforced_return_checking.md) — Паттерн 17: Принудительная проверка возврата (атрибуты `[[nodiscard]]` и `warn_unused_result`).

**Внешние стандарты и гайдлайны:**

- **SEI CERT C Coding Standard:**
  - [ERR30-C](https://wiki.sei.cmu.edu/confluence/display/c/ERR30-C) (Take care when reading errno).
  - [ERR33-C](https://wiki.sei.cmu.edu/confluence/display/c/ERR33-C) (Detect and handle standard library errors).
- **POSIX.1-2017 Standard:** Раздел 2.3 (Error Numbers) и описание `strerror_r` / `strerror_l`.
- **ISO/IEC 9899:2011 (C11 Standard):** Раздел 7.5 (Errors `<errno.h>`) — строгое ограничение гарантированных кодов.
- **ISO/IEC 9899:2024 (C23 Standard):** Раздел по атрибутам `[[nodiscard]]`.
- **Linux Kernel Coding Style:** Раздел о возврате кодов ошибок и маппинге `errno` во внутренние коды подсистем.

## Changelog

  v1.0 — initial.
