# Изменения

## Русский

Исправления в `DvolApp.c`:

- Утилита теперь обрабатывает только физические диски и пропускает `LogicalPartition`, чтобы не затирать отдельные разделы вместо таблицы разделов всего носителя.
- Индексация дисков в списке сканирования сделана стабильной и соответствующей отображаемому списку.
- Разбор `-d/--disk` усилен: добавлена строгая проверка числового значения, обработка отсутствующего аргумента и защита от слишком большого числа целей.
- Подтверждение операции исправлено: `WaitForEvent()` теперь вызывается с корректным выходным индексом события.
- Логика работы с GPT переработана:
  - добавлена проверка первичного GPT-заголовка;
  - размер массива GPT entry вычисляется безопасно;
  - диапазон backup GPT entry array теперь считается по числу блоков, а не по числу записей;
  - при очистке MBR теперь зануляется весь `LBA0`, а не только записи разделов;
  - при очистке GPT теперь зануляются весь `LBA0`, primary GPT header, primary GPT entry array, backup GPT entry array и backup GPT header, чтобы диск оставался полностью неразмеченным.
- Подсчёт GPT-разделов теперь учитывает реальный шаг `SizeOfPartitionEntry`, а не предполагает плотный массив `EFI_PARTITION_ENTRY`.
- Логирование улучшено:
  - вывод в файл переведён на shell-aware открытие пути через `EFI_SHELL_PROTOCOL`;
  - исправлена запись `CRLF` в лог;
  - сохранена запись UTF-16 BOM в начале файла.
- Вывод справки и обработка неизвестных аргументов сделаны более строгими и понятными.

Проверка:

- Сборка успешно проверена через `build_minimal.bat` с окружением `VS2026`.
- Собранный бинарник: `C:\edk2workspace2\Build\DvolPkg\RELEASE_VS2026\X64\dvol.efi`.

## English

Changes made in `DvolApp.c`:

- The utility now processes only whole physical disks and skips `LogicalPartition` handles, preventing accidental writes to individual partitions instead of the disk partition table.
- Disk indexing in the scan output was made stable and consistent with the displayed list.
- `-d/--disk` parsing was hardened: strict numeric validation was added, missing values are rejected, and too many targets are handled explicitly.
- The confirmation flow was fixed: `WaitForEvent()` now receives a valid event index output pointer.
- GPT handling was reworked:
  - primary GPT header validation was added;
  - GPT entry array sizing is now computed safely;
  - the backup GPT entry array range is calculated in blocks instead of raw entry count;
  - MBR cleanup now zeroes the entire `LBA0` instead of only the partition entries;
  - GPT cleanup now zeroes the entire `LBA0`, the primary GPT header, the primary GPT entry array, the backup GPT entry array, and the backup GPT header so the disk is left fully unpartitioned.
- GPT partition counting now respects the real `SizeOfPartitionEntry` stride instead of assuming a tightly packed `EFI_PARTITION_ENTRY` array.
- Logging was improved:
  - file output now uses shell-aware path opening via `EFI_SHELL_PROTOCOL`;
  - `CRLF` writing to the log file was fixed;
  - the UTF-16 BOM is still written at the beginning of the file.
- Help text and handling of invalid command-line arguments were made stricter and clearer.

Verification:

- The build was successfully verified via `build_minimal.bat` with a `VS2026` environment.
- Built binary: `C:\edk2workspace2\Build\DvolPkg\RELEASE_VS2026\X64\dvol.efi`.
