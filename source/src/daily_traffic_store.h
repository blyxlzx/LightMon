#pragma once

#include "daily_traffic_counter.h"
#include <windows.h>
#include <cstddef>
#include <cstdint>

namespace lightmon {

enum class DailyTrafficStoreStatus : std::uint32_t {
    MemoryOnly, Missing, Loaded, InvalidFile, Saved, WriteFailed
};

// Small local checkpoint, not a history database. Only committed daily totals
// are restored: raw NIC counters are deliberately never carried across restart.
// No allocation, worker, service, or per-second disk writes.
class DailyTrafficStore {
public:
    bool Configure(const wchar_t* path, DailyTrafficTotals& loaded) noexcept {
        loaded = DailyTrafficTotals{};
        path_[0] = temporary_[0] = L'\0';
        saved_ = DailyTrafficTotals{};
        status_ = DailyTrafficStoreStatus::MemoryOnly;
        error_ = 0;
        attentionDate_ = 0;
        lastAttemptTick_ = 0;
        attemptDate_ = 0;
        attention_ = savedAttention_ = attempted_ = haveSaved_ = false;
        memoryOnly_ = path == nullptr;
        pathValid_ = false;
        if (memoryOnly_) return false;
        if (!SetPath(path)) {
            Fail(DailyTrafficStoreStatus::WriteFailed, ERROR_BAD_PATHNAME);
            return false;
        }
        pathValid_ = true;
        HANDLE file = CreateFileW(path_, GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            const DWORD failure = GetLastError();
            if (failure == ERROR_FILE_NOT_FOUND || failure == ERROR_PATH_NOT_FOUND) {
                status_ = DailyTrafficStoreStatus::Missing;
                error_ = failure;
            } else {
                Fail(DailyTrafficStoreStatus::InvalidFile, failure);
            }
            return false;
        }
        unsigned char record[kRecordBytes]{};
        LARGE_INTEGER size{};
        DWORD bytes = 0;
        const bool sized = GetFileSizeEx(file, &size) != FALSE;
        const DWORD sizeError = sized ? ERROR_SUCCESS : GetLastError();
        bool read = false;
        DWORD readError = ERROR_SUCCESS;
        if (sized && size.QuadPart == static_cast<LONGLONG>(kRecordBytes)) {
            read = ReadFile(file, record, kRecordBytes, &bytes, nullptr) != FALSE;
            if (!read) readError = GetLastError();
        }
        CloseHandle(file);
        if (!sized || !read || bytes != kRecordBytes) {
            Fail(DailyTrafficStoreStatus::InvalidFile,
                !sized ? sizeError : (readError ? readError : ERROR_INVALID_DATA));
            return false;
        }
        bool recordedAttention = false;
        if (!Decode(record, loaded, recordedAttention)) {
            loaded = DailyTrafficTotals{};
            Fail(DailyTrafficStoreStatus::InvalidFile, ERROR_INVALID_DATA);
            return false;
        }
        saved_ = loaded;
        savedAttention_ = attention_ = recordedAttention;
        attentionDate_ = attention_ ? loaded.date : 0;
        haveSaved_ = attempted_ = true;
        attemptDate_ = loaded.date;
        lastAttemptTick_ = GetTickCount64();
        status_ = DailyTrafficStoreStatus::Loaded;
        error_ = 0;
        return true;
    }

    // First checkpoint and every date change are immediate. Normal changed
    // totals are checkpointed at most once per 30 seconds; failed retries use
    // the same interval. force is for orderly exit/suspend/session termination.
    void Save(const DailyTrafficTotals& totals, bool force = false) noexcept {
        if (memoryOnly_) return;
        if (!pathValid_) {
            Fail(DailyTrafficStoreStatus::WriteFailed, ERROR_BAD_PATHNAME);
            return;
        }
        if (!ValidDate(totals.date)) {
            Fail(DailyTrafficStoreStatus::WriteFailed, ERROR_INVALID_DATA);
            return;
        }
        if (attention_) {
            if (attentionDate_ == 0) attentionDate_ = totals.date;
            else if (attentionDate_ != totals.date) {
                attention_ = false;
                attentionDate_ = 0;
            }
        }
        if (!force && haveSaved_ && Equal(saved_, totals) &&
            savedAttention_ == attention_) return;
        const ULONGLONG now = GetTickCount64();
        if (!force && attempted_ && attemptDate_ == totals.date &&
            now >= lastAttemptTick_ && now - lastAttemptTick_ < kSaveIntervalMs)
            return;
        attempted_ = true;
        attemptDate_ = totals.date;
        lastAttemptTick_ = now;
        unsigned char record[kRecordBytes]{};
        Encode(totals, attention_, record);
        HANDLE file = CreateFileW(temporary_, GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            attentionDate_ = totals.date;
            Fail(DailyTrafficStoreStatus::WriteFailed, GetLastError());
            return;
        }
        DWORD bytes = 0;
        const bool written = WriteFile(file, record, kRecordBytes, &bytes, nullptr) != FALSE;
        DWORD failure = written ? ERROR_SUCCESS : GetLastError();
        if (written && bytes != kRecordBytes) failure = ERROR_WRITE_FAULT;
        if (failure == ERROR_SUCCESS && !FlushFileBuffers(file)) failure = GetLastError();
        if (!CloseHandle(file) && failure == ERROR_SUCCESS) failure = GetLastError();
        if (failure == ERROR_SUCCESS && !MoveFileExW(temporary_, path_,
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            failure = GetLastError();
        if (failure != ERROR_SUCCESS) {
            // The previous checkpoint is untouched when writing/flush fails.
            // A replacement failure keeps the previous file and is reported.
            DeleteFileW(temporary_);
            attentionDate_ = totals.date;
            Fail(DailyTrafficStoreStatus::WriteFailed, failure);
            return;
        }
        saved_ = totals;
        savedAttention_ = attention_;
        haveSaved_ = true;
        status_ = DailyTrafficStoreStatus::Saved;
        error_ = 0;
    }

    DailyTrafficStoreStatus Status() const noexcept { return status_; }
    std::uint32_t Error() const noexcept { return error_; }
    bool NeedsAttention() const noexcept { return attention_; }

private:
    static constexpr DWORD kRecordBytes = 48;
    static constexpr ULONGLONG kSaveIntervalMs = 30000;
    // Little-endian layout: magic[8], version32, YYYYMMDD32, down64, up64,
    // flags32 (bit 0 = incomplete day), reserved32=0, FNV-1a64(first 40 bytes).
    static constexpr unsigned char kMagic[8] = {'L','M','T','R','A','F','1',0};

    static bool Equal(const DailyTrafficTotals& a, const DailyTrafficTotals& b) noexcept {
        return a.date == b.date && a.downloadBytes == b.downloadBytes &&
               a.uploadBytes == b.uploadBytes;
    }
    static bool ValidDate(std::uint32_t date) noexcept {
        const std::uint32_t year = date / 10000;
        const std::uint32_t month = date / 100 % 100;
        const std::uint32_t day = date % 100;
        if (year < 1601 || year > 9999 || month < 1 || month > 12 || day < 1)
            return false;
        constexpr unsigned days[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
        unsigned maximum = days[month - 1];
        if (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))
            ++maximum;
        return day <= maximum;
    }
    static void Put(unsigned char* output, std::uint64_t value, unsigned length) noexcept {
        for (unsigned i = 0; i < length; ++i) {
            output[i] = static_cast<unsigned char>(value & 0xff);
            value >>= 8;
        }
    }
    static std::uint64_t Get(const unsigned char* input, unsigned length) noexcept {
        std::uint64_t value = 0;
        for (unsigned i = 0; i < length; ++i)
            value |= static_cast<std::uint64_t>(input[i]) << (8 * i);
        return value;
    }
    static std::uint64_t Checksum(const unsigned char* record) noexcept {
        std::uint64_t hash = 14695981039346656037ULL;
        for (unsigned i = 0; i < 40; ++i) {
            hash ^= record[i];
            hash *= 1099511628211ULL;
        }
        return hash;
    }
    static void Encode(const DailyTrafficTotals& totals, bool attention,
                       unsigned char (&record)[kRecordBytes]) noexcept {
        for (unsigned i = 0; i < 8; ++i) record[i] = kMagic[i];
        Put(record + 8, 1, 4);
        Put(record + 12, totals.date, 4);
        Put(record + 16, totals.downloadBytes, 8);
        Put(record + 24, totals.uploadBytes, 8);
        Put(record + 32, attention ? 1 : 0, 4);
        Put(record + 40, Checksum(record), 8);
    }
    static bool Decode(const unsigned char (&record)[kRecordBytes],
                       DailyTrafficTotals& totals, bool& attention) noexcept {
        for (unsigned i = 0; i < 8; ++i) if (record[i] != kMagic[i]) return false;
        const auto date = static_cast<std::uint32_t>(Get(record + 12, 4));
        const std::uint64_t flags = Get(record + 32, 4);
        if (Get(record + 8, 4) != 1 || !ValidDate(date) || flags > 1 ||
            Get(record + 36, 4) != 0 || Get(record + 40, 8) != Checksum(record))
            return false;
        totals = DailyTrafficTotals{date, Get(record + 16, 8), Get(record + 24, 8)};
        attention = flags != 0;
        return true;
    }
    bool SetPath(const wchar_t* input) noexcept {
        std::size_t length = 0;
        while (length < MAX_PATH && input[length]) ++length;
        if (length == 0 || length > MAX_PATH - 5) return false;
        const bool drive = length >= 3 &&
            ((input[0] >= L'A' && input[0] <= L'Z') ||
             (input[0] >= L'a' && input[0] <= L'z')) && input[1] == L':' &&
            (input[2] == L'\\' || input[2] == L'/');
        const bool unc = length >= 5 && input[0] == L'\\' && input[1] == L'\\' &&
            input[2] != L'.' && input[2] != L'?';
        if (!drive && !unc) return false;
        for (std::size_t i = 0; i < length; ++i) {
            const wchar_t c = input[i];
            if (c < L' ' || c == L'"' || c == L'<' || c == L'>' || c == L'|' ||
                c == L'*' || c == L'?' || (c == L':' && (!drive || i != 1)))
                return false;
        }
        wchar_t* filename = nullptr;
        const DWORD fullLength = GetFullPathNameW(input, MAX_PATH, path_, &filename);
        if (fullLength == 0 || fullLength > MAX_PATH - 5 || !filename || !*filename)
            return false;
        if (path_[fullLength - 1] == L'.' || path_[fullLength - 1] == L' ') return false;
        for (DWORD i = 0; i < fullLength; ++i) temporary_[i] = path_[i];
        temporary_[fullLength] = L'.';
        temporary_[fullLength + 1] = L't';
        temporary_[fullLength + 2] = L'm';
        temporary_[fullLength + 3] = L'p';
        temporary_[fullLength + 4] = L'\0';
        return true;
    }
    void Fail(DailyTrafficStoreStatus status, DWORD error) noexcept {
        status_ = status;
        error_ = error ? error : ERROR_INVALID_DATA;
        attention_ = true;
    }

    wchar_t path_[MAX_PATH]{};
    wchar_t temporary_[MAX_PATH]{};
    DailyTrafficTotals saved_{};
    DailyTrafficStoreStatus status_ = DailyTrafficStoreStatus::MemoryOnly;
    std::uint32_t error_ = 0;
    std::uint32_t attentionDate_ = 0;
    std::uint32_t attemptDate_ = 0;
    ULONGLONG lastAttemptTick_ = 0;
    bool memoryOnly_ = true;
    bool pathValid_ = false;
    bool attention_ = false;
    bool savedAttention_ = false;
    bool attempted_ = false;
    bool haveSaved_ = false;
};

} // namespace lightmon
