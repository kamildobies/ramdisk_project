# RAM-Disk FUSE - Raport z Projektu

Projekt stanowi implementację dysku wirtualnego z systemem plików działającym w przestrzeni użytkownika (**FUSE** - *Filesystem in Userspace*). Całość danych oraz metadanych przechowywana jest w pamięci RAM, przez co giną one po zamknięciu programu.

## 1. Architektura i Struktura Danych

System opiera się na statycznej tablicy struktur, która przechowuje hierarchię plików i katalogów.

### 1.1 Model Pamięci

Podstawową jednostką jest struktura `MyFile`, która łączy metadane z buforem danych.

```c
struct MyFile {
    char name[256];         // Nazwa pliku lub katalogu
    mode_t mode;            // Uprawnienia i typ (S_IFREG/S_IFDIR)
    size_t size;            // Rozmiar w bajtach
    int parent_index;       // Indeks rodzica w tabeli
    char data_buffer[MAX_FILE_SIZE];    // Stały bufor na dane pliku
};

```

* **`MAX_FILES` (100)**: Maksymalna liczba obiektów (plików/katalogów).
* **`MAX_FILE_SIZE` (4096)**: Maksymalny rozmiar pojedynczego pliku (4KB).

---

## 2. Kluczowe Funkcje Systemowe

### 2.1 Nawigacja i Ścieżki

System wykorzystuje dwie główne funkcje do translacji ścieżek tekstowych na indeksy tabeli:

* **`resolve_path_to_index`**: Przeszukuje tabelę `file_table` token po tokenie (np. najpierw szuka `/home`, potem `/home/user`).
* **`resolve_parent_and_name`**: Rozbija ścieżkę na lokalizację rodzica oraz nazwę nowego pliku (wykorzystywane przy `mkdir` i `create`).

### 2.2 Diagnostyka i Logowanie (`log_msg`)

Program implementuje mechanizm logowania. Logi trafiają do dwóch miejsc:

1. **Standard Error (`stderr`)**: Widoczne w terminalu aplikacji.
2. **Kernel Log (`/dev/kmsg`)**: Widoczne w systemowym poleceniu `dmesg`.

| Poziom | Prefiks | Zastosowanie |
| --- | --- | --- |
| `LOG_ERROR` | `<3>` | Błędy operacji (np. `-ENOSPC`, `-EEXIST`) |
| `LOG_INFO` | `<6>` | Tworzenie/Usuwanie plików, montowanie |
| `LOG_DEBUG` | `<7>` | Szczegółowe trasowanie ścieżek |

---

## 3. Implementacja Operacji FUSE

| Operacja | Opis |
| --- | --- |
| `.getattr` | Pobiera atrybuty pliku (rozmiar, uprawnienia). |
| `.readdir` | Wyświetla zawartość katalogu (obsługuje `.` i `..`). |
| `.create` / `.mkdir` | Alokuje nowy slot w tabeli i inicjalizuje metadane. |
| `.read` / `.write` | Kopiuje dane między buforem procesu a `data_buffer`. |
| `.rmdir` | Usuwa katalog z tabeli (wymaga, aby katalog był wcześniej pusty). |
| `.unlink` | Usuwa plik regularny poprzez wyzerowanie jego nazwy w `file_table`. |
| `.truncate` | Zmienia rozmiar pliku; przy skróceniu do 0 dodatkowo czyści początek bufora. |
| `.statfs` | Raportuje zajętość miejsca widoczną w komendzie `df -T`. |

---

## 4. Instrukcja Obsługi

### 4.1 Kompilacja

Wymagane jest zainstalowane środowisko FUSE (`libfuse-dev`).

```bash
gcc -Wall -D_FILE_OFFSET_BITS=64 FUSE/fuse_ramdisk.c -lfuse -o FUSE/fuse_ramdisk

```

### 4.2 Uruchomienie

Użycie flagi `-f` pozwala na podgląd logów w czasie rzeczywistym.

```bash
# Odmontowanie poprzedniej sesji i przygotowanie folderu
sudo fusermount -u /tmp/mojdysk 2>/dev/null
mkdir -p /tmp/mojdysk

# Uruchomienie ramdysku
sudo ./FUSE/fuse_ramdisk -o allow_other -f /tmp/mojdysk

```

### 4.3 Monitorowanie Logów

W osobnym terminalu można obserwować komunikaty przesyłane do jądra:

```bash
sudo dmesg -w | grep fuse_ramdisk

```

---

## 5. Ograniczenia i Uwagi Techniczne

1. **Brak dynamicznej alokacji**: Każdy plik rezerwuje 4 KB RAM, nawet jeśli jest pusty.
2. **Brak persystencji**: Po zabiciu procesu (np. `Ctrl+C`), dane są usuwane.
3. **Współbieżność**: System nie implementuje mutexów (blokad), co może prowadzić do *race conditions* przy jednoczesnym dostępie wielu procesów.