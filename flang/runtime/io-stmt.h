//===-- runtime/io-stmt.h ---------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// Representations of the state of an I/O statement in progress

#ifndef FORTRAN_RUNTIME_IO_STMT_H_
#define FORTRAN_RUNTIME_IO_STMT_H_

#include "connection.h"
#include "file.h"
#include "format.h"
#include "internal-unit.h"
#include "io-error.h"
#include "flang/Common/visit.h"
#include "flang/Runtime/descriptor.h"
#include "flang/Runtime/io-api.h"
#include <functional>
#include <type_traits>
#include <variant>

namespace Fortran::runtime::io {

class ExternalFileUnit;
class ChildIo;

class OpenStatementState;
class InquireUnitState;
class InquireNoUnitState;
class InquireUnconnectedFileState;
class InquireIOLengthState;
class ExternalMiscIoStatementState;
class CloseStatementState;
class NoopStatementState; // CLOSE or FLUSH on unknown unit
class ErroneousIoStatementState;

template <Direction, typename CHAR = char>
class InternalFormattedIoStatementState;
template <Direction> class InternalListIoStatementState;
template <Direction, typename CHAR = char>
class ExternalFormattedIoStatementState;
template <Direction> class ExternalListIoStatementState;
template <Direction> class ExternalUnformattedIoStatementState;
template <Direction, typename CHAR = char> class ChildFormattedIoStatementState;
template <Direction> class ChildListIoStatementState;
template <Direction> class ChildUnformattedIoStatementState;

struct InputStatementState {};
struct OutputStatementState {};
template <Direction D>
using IoDirectionState = std::conditional_t<D == Direction::Input,
    InputStatementState, OutputStatementState>;

// Common state for all kinds of formatted I/O
template <Direction D> class FormattedIoStatementState {};
template <> class FormattedIoStatementState<Direction::Input> {
public:
  std::size_t GetEditDescriptorChars() const;
  void GotChar(int);

private:
  // Account of characters read for edit descriptors (i.e., formatted I/O
  // with a FORMAT, not list-directed or NAMELIST), not including padding.
  std::size_t chars_{0}; // for READ(SIZE=)
};

// The Cookie type in the I/O API is a pointer (for C) to this class.
class IoStatementState {
public:
  template <typename A> explicit IoStatementState(A &x) : u_{x} {}

  // These member functions each project themselves into the active alternative.
  // They're used by per-data-item routines in the I/O API (e.g., OutputReal64)
  // to interact with the state of the I/O statement in progress.
  // This design avoids virtual member functions and function pointers,
  // which may not have good support in some runtime environments.

  // CompleteOperation() is the last opportunity to raise an I/O error.
  // It is called by EndIoStatement(), but it can be invoked earlier to
  // catch errors for (e.g.) GetIoMsg() and GetNewUnit().  If called
  // more than once, it is a no-op.
  void CompleteOperation();
  // Completes an I/O statement and reclaims storage.
  int EndIoStatement();

  bool Emit(const char *, std::size_t bytes, std::size_t elementBytes = 0);
  bool Receive(char *, std::size_t, std::size_t elementBytes = 0);
  std::size_t GetNextInputBytes(const char *&);
  bool AdvanceRecord(int = 1);
  void BackspaceRecord();
  void HandleRelativePosition(std::int64_t);
  void HandleAbsolutePosition(std::int64_t); // for r* in list I/O
  std::optional<DataEdit> GetNextDataEdit(int maxRepeat = 1);
  ExternalFileUnit *GetExternalFileUnit() const; // null if internal unit
  bool BeginReadingRecord();
  void FinishReadingRecord();
  bool Inquire(InquiryKeywordHash, char *, std::size_t);
  bool Inquire(InquiryKeywordHash, bool &);
  bool Inquire(InquiryKeywordHash, std::int64_t, bool &); // PENDING=
  bool Inquire(InquiryKeywordHash, std::int64_t &);
  void GotChar(signed int = 1); // for READ(SIZE=); can be <0

  MutableModes &mutableModes();
  ConnectionState &GetConnectionState();
  IoErrorHandler &GetIoErrorHandler() const;

  // N.B.: this also works with base classes
  template <typename A> A *get_if() const {
    return common::visit(
        [](auto &x) -> A * {
          if constexpr (std::is_convertible_v<decltype(x.get()), A &>) {
            return &x.get();
          }
          return nullptr;
        },
        u_);
  }

  // Vacant after the end of the current record
  std::optional<char32_t> GetCurrentChar(std::size_t &byteCount);

  // For fixed-width fields, return the number of remaining characters.
  // Skip over leading blanks.
  std::optional<int> CueUpInput(const DataEdit &edit) {
    std::optional<int> remaining;
    if (edit.IsListDirected()) {
      std::size_t byteCount{0};
      GetNextNonBlank(byteCount);
    } else {
      if (edit.width.value_or(0) > 0) {
        remaining = *edit.width;
      }
      SkipSpaces(remaining);
    }
    return remaining;
  }

  std::optional<char32_t> SkipSpaces(std::optional<int> &remaining) {
    while (!remaining || *remaining > 0) {
      std::size_t byteCount{0};
      if (auto ch{GetCurrentChar(byteCount)}) {
        if (*ch != ' ' && *ch != '\t') {
          return ch;
        }
        if (remaining) {
          if (static_cast<std::size_t>(*remaining) < byteCount) {
            break;
          }
          GotChar(byteCount);
          *remaining -= byteCount;
        }
        HandleRelativePosition(byteCount);
      } else {
        break;
      }
    }
    return std::nullopt;
  }

  // Acquires the next input character, respecting any applicable field width
  // or separator character.
  std::optional<char32_t> NextInField(
      std::optional<int> &remaining, const DataEdit &);

  // Detect and signal any end-of-record condition after input.
  // Returns true if at EOR and remaining input should be padded with blanks.
  bool CheckForEndOfRecord();

  // Skips spaces, advances records, and ignores NAMELIST comments
  std::optional<char32_t> GetNextNonBlank(std::size_t &byteCount) {
    auto ch{GetCurrentChar(byteCount)};
    bool inNamelist{mutableModes().inNamelist};
    while (!ch || *ch == ' ' || *ch == '\t' || (inNamelist && *ch == '!')) {
      if (ch && (*ch == ' ' || *ch == '\t')) {
        HandleRelativePosition(byteCount);
      } else if (!AdvanceRecord()) {
        return std::nullopt;
      }
      ch = GetCurrentChar(byteCount);
    }
    return ch;
  }

  template <Direction D> bool CheckFormattedStmtType(const char *name) {
    if (get_if<FormattedIoStatementState<D>>()) {
      return true;
    } else {
      auto &handler{GetIoErrorHandler()};
      if (!handler.InError()) {
        handler.Crash("%s called for I/O statement that is not formatted %s",
            name, D == Direction::Output ? "output" : "input");
      }
      return false;
    }
  }

private:
  std::variant<std::reference_wrapper<OpenStatementState>,
      std::reference_wrapper<CloseStatementState>,
      std::reference_wrapper<NoopStatementState>,
      std::reference_wrapper<
          InternalFormattedIoStatementState<Direction::Output>>,
      std::reference_wrapper<
          InternalFormattedIoStatementState<Direction::Input>>,
      std::reference_wrapper<InternalListIoStatementState<Direction::Output>>,
      std::reference_wrapper<InternalListIoStatementState<Direction::Input>>,
      std::reference_wrapper<
          ExternalFormattedIoStatementState<Direction::Output>>,
      std::reference_wrapper<
          ExternalFormattedIoStatementState<Direction::Input>>,
      std::reference_wrapper<ExternalListIoStatementState<Direction::Output>>,
      std::reference_wrapper<ExternalListIoStatementState<Direction::Input>>,
      std::reference_wrapper<
          ExternalUnformattedIoStatementState<Direction::Output>>,
      std::reference_wrapper<
          ExternalUnformattedIoStatementState<Direction::Input>>,
      std::reference_wrapper<ChildFormattedIoStatementState<Direction::Output>>,
      std::reference_wrapper<ChildFormattedIoStatementState<Direction::Input>>,
      std::reference_wrapper<ChildListIoStatementState<Direction::Output>>,
      std::reference_wrapper<ChildListIoStatementState<Direction::Input>>,
      std::reference_wrapper<
          ChildUnformattedIoStatementState<Direction::Output>>,
      std::reference_wrapper<
          ChildUnformattedIoStatementState<Direction::Input>>,
      std::reference_wrapper<InquireUnitState>,
      std::reference_wrapper<InquireNoUnitState>,
      std::reference_wrapper<InquireUnconnectedFileState>,
      std::reference_wrapper<InquireIOLengthState>,
      std::reference_wrapper<ExternalMiscIoStatementState>,
      std::reference_wrapper<ErroneousIoStatementState>>
      u_;
};

// Base class for all per-I/O statement state classes.
class IoStatementBase : public IoErrorHandler {
public:
  using IoErrorHandler::IoErrorHandler;

  bool completedOperation() const { return completedOperation_; }

  void CompleteOperation() { completedOperation_ = true; }
  int EndIoStatement() { return GetIoStat(); }

  // These are default no-op backstops that can be overridden by descendants.
  bool Emit(const char *, std::size_t bytes, std::size_t elementBytes = 0);
  bool Receive(char *, std::size_t bytes, std::size_t elementBytes = 0);
  std::size_t GetNextInputBytes(const char *&);
  bool AdvanceRecord(int);
  void BackspaceRecord();
  void HandleRelativePosition(std::int64_t);
  void HandleAbsolutePosition(std::int64_t);
  std::optional<DataEdit> GetNextDataEdit(
      IoStatementState &, int maxRepeat = 1);
  ExternalFileUnit *GetExternalFileUnit() const;
  bool BeginReadingRecord();
  void FinishReadingRecord();
  bool Inquire(InquiryKeywordHash, char *, std::size_t);
  bool Inquire(InquiryKeywordHash, bool &);
  bool Inquire(InquiryKeywordHash, std::int64_t, bool &);
  bool Inquire(InquiryKeywordHash, std::int64_t &);

  void BadInquiryKeywordHashCrash(InquiryKeywordHash);

protected:
  bool completedOperation_{false};
};

// Common state for list-directed & NAMELIST I/O, both internal & external
template <Direction> class ListDirectedStatementState;
template <>
class ListDirectedStatementState<Direction::Output>
    : public FormattedIoStatementState<Direction::Output> {
public:
  bool EmitLeadingSpaceOrAdvance(
      IoStatementState &, std::size_t = 1, bool isCharacter = false);
  std::optional<DataEdit> GetNextDataEdit(
      IoStatementState &, int maxRepeat = 1);
  bool lastWasUndelimitedCharacter() const {
    return lastWasUndelimitedCharacter_;
  }
  void set_lastWasUndelimitedCharacter(bool yes = true) {
    lastWasUndelimitedCharacter_ = yes;
  }

private:
  bool lastWasUndelimitedCharacter_{false};
};
template <>
class ListDirectedStatementState<Direction::Input>
    : public FormattedIoStatementState<Direction::Input> {
public:
  bool inNamelistArray() const { return inNamelistArray_; }
  void set_inNamelistArray(bool yes = true) { inNamelistArray_ = yes; }

  // Skips value separators, handles repetition and null values.
  // Vacant when '/' appears; present with descriptor == ListDirectedNullValue
  // when a null value appears.
  std::optional<DataEdit> GetNextDataEdit(
      IoStatementState &, int maxRepeat = 1);

  // Each NAMELIST input item is treated like a distinct list-directed
  // input statement.  This member function resets some state so that
  // repetition and null values work correctly for each successive
  // NAMELIST input item.
  void ResetForNextNamelistItem(bool inNamelistArray) {
    remaining_ = 0;
    eatComma_ = false;
    realPart_ = imaginaryPart_ = false;
    inNamelistArray_ = inNamelistArray;
  }

private:
  int remaining_{0}; // for "r*" repetition
  std::optional<SavedPosition> repeatPosition_;
  bool eatComma_{false}; // consume comma after previously read item
  bool hitSlash_{false}; // once '/' is seen, nullify further items
  bool realPart_{false};
  bool imaginaryPart_{false};
  bool inNamelistArray_{false};
};

template <Direction DIR>
class InternalIoStatementState : public IoStatementBase,
                                 public IoDirectionState<DIR> {
public:
  using Buffer =
      std::conditional_t<DIR == Direction::Input, const char *, char *>;
  InternalIoStatementState(Buffer, std::size_t,
      const char *sourceFile = nullptr, int sourceLine = 0);
  InternalIoStatementState(
      const Descriptor &, const char *sourceFile = nullptr, int sourceLine = 0);
  int EndIoStatement();

  bool Emit(const char *data, std::size_t bytes, std::size_t elementBytes = 0);
  std::size_t GetNextInputBytes(const char *&);
  bool AdvanceRecord(int = 1);
  void BackspaceRecord();
  ConnectionState &GetConnectionState() { return unit_; }
  MutableModes &mutableModes() { return unit_.modes; }
  void HandleRelativePosition(std::int64_t);
  void HandleAbsolutePosition(std::int64_t);

protected:
  bool free_{true};
  InternalDescriptorUnit<DIR> unit_;
};

template <Direction DIR, typename CHAR>
class InternalFormattedIoStatementState
    : public InternalIoStatementState<DIR>,
      public FormattedIoStatementState<DIR> {
public:
  using CharType = CHAR;
  using typename InternalIoStatementState<DIR>::Buffer;
  InternalFormattedIoStatementState(Buffer internal, std::size_t internalLength,
      const CharType *format, std::size_t formatLength,
      const char *sourceFile = nullptr, int sourceLine = 0,
      const Descriptor *formatDescriptor = nullptr);
  InternalFormattedIoStatementState(const Descriptor &, const CharType *format,
      std::size_t formatLength, const char *sourceFile = nullptr,
      int sourceLine = 0, const Descriptor *formatDescriptor = nullptr);
  IoStatementState &ioStatementState() { return ioStatementState_; }
  void CompleteOperation();
  int EndIoStatement();
  std::optional<DataEdit> GetNextDataEdit(
      IoStatementState &, int maxRepeat = 1) {
    return format_.GetNextDataEdit(*this, maxRepeat);
  }

private:
  IoStatementState ioStatementState_; // points to *this
  using InternalIoStatementState<DIR>::unit_;
  // format_ *must* be last; it may be partial someday
  FormatControl<InternalFormattedIoStatementState> format_;
};

template <Direction DIR>
class InternalListIoStatementState : public InternalIoStatementState<DIR>,
                                     public ListDirectedStatementState<DIR> {
public:
  using typename InternalIoStatementState<DIR>::Buffer;
  InternalListIoStatementState(Buffer internal, std::size_t internalLength,
      const char *sourceFile = nullptr, int sourceLine = 0);
  InternalListIoStatementState(
      const Descriptor &, const char *sourceFile = nullptr, int sourceLine = 0);
  IoStatementState &ioStatementState() { return ioStatementState_; }
  using ListDirectedStatementState<DIR>::GetNextDataEdit;

private:
  IoStatementState ioStatementState_; // points to *this
  using InternalIoStatementState<DIR>::unit_;
};

class ExternalIoStatementBase : public IoStatementBase {
public:
  ExternalIoStatementBase(
      ExternalFileUnit &, const char *sourceFile = nullptr, int sourceLine = 0);
  ExternalFileUnit &unit() { return unit_; }
  MutableModes &mutableModes();
  ConnectionState &GetConnectionState();
  int asynchronousID() const { return asynchronousID_; }
  int EndIoStatement();
  ExternalFileUnit *GetExternalFileUnit() const { return &unit_; }
  void SetAsynchronous();

private:
  ExternalFileUnit &unit_;
  int asynchronousID_{-1};
};

template <Direction DIR>
class ExternalIoStatementState : public ExternalIoStatementBase,
                                 public IoDirectionState<DIR> {
public:
  ExternalIoStatementState(
      ExternalFileUnit &, const char *sourceFile = nullptr, int sourceLine = 0);
  MutableModes &mutableModes() { return mutableModes_; }
  void CompleteOperation();
  int EndIoStatement();
  bool Emit(const char *, std::size_t bytes, std::size_t elementBytes = 0);
  std::size_t GetNextInputBytes(const char *&);
  bool AdvanceRecord(int = 1);
  void BackspaceRecord();
  void HandleRelativePosition(std::int64_t);
  void HandleAbsolutePosition(std::int64_t);
  bool BeginReadingRecord();
  void FinishReadingRecord();

private:
  // These are forked from ConnectionState's modes at the beginning
  // of each formatted I/O statement so they may be overridden by control
  // edit descriptors during the statement.
  MutableModes mutableModes_;
};

template <Direction DIR, typename CHAR>
class ExternalFormattedIoStatementState
    : public ExternalIoStatementState<DIR>,
      public FormattedIoStatementState<DIR> {
public:
  using CharType = CHAR;
  ExternalFormattedIoStatementState(ExternalFileUnit &, const CharType *format,
      std::size_t formatLength, const char *sourceFile = nullptr,
      int sourceLine = 0, const Descriptor *formatDescriptor = nullptr);
  void CompleteOperation();
  int EndIoStatement();
  std::optional<DataEdit> GetNextDataEdit(
      IoStatementState &, int maxRepeat = 1) {
    return format_.GetNextDataEdit(*this, maxRepeat);
  }

private:
  FormatControl<ExternalFormattedIoStatementState> format_;
};

template <Direction DIR>
class ExternalListIoStatementState : public ExternalIoStatementState<DIR>,
                                     public ListDirectedStatementState<DIR> {
public:
  using ExternalIoStatementState<DIR>::ExternalIoStatementState;
  using ListDirectedStatementState<DIR>::GetNextDataEdit;
};

template <Direction DIR>
class ExternalUnformattedIoStatementState
    : public ExternalIoStatementState<DIR> {
public:
  using ExternalIoStatementState<DIR>::ExternalIoStatementState;
  bool Receive(char *, std::size_t, std::size_t elementBytes = 0);
};

template <Direction DIR>
class ChildIoStatementState : public IoStatementBase,
                              public IoDirectionState<DIR> {
public:
  ChildIoStatementState(
      ChildIo &, const char *sourceFile = nullptr, int sourceLine = 0);
  ChildIo &child() { return child_; }
  MutableModes &mutableModes();
  ConnectionState &GetConnectionState();
  ExternalFileUnit *GetExternalFileUnit() const;
  void CompleteOperation();
  int EndIoStatement();
  bool Emit(const char *, std::size_t bytes, std::size_t elementBytes = 0);
  std::size_t GetNextInputBytes(const char *&);
  void HandleRelativePosition(std::int64_t);
  void HandleAbsolutePosition(std::int64_t);

private:
  ChildIo &child_;
};

template <Direction DIR, typename CHAR>
class ChildFormattedIoStatementState : public ChildIoStatementState<DIR>,
                                       public FormattedIoStatementState<DIR> {
public:
  using CharType = CHAR;
  ChildFormattedIoStatementState(ChildIo &, const CharType *format,
      std::size_t formatLength, const char *sourceFile = nullptr,
      int sourceLine = 0, const Descriptor *formatDescriptor = nullptr);
  MutableModes &mutableModes() { return mutableModes_; }
  void CompleteOperation();
  int EndIoStatement();
  bool AdvanceRecord(int = 1);
  std::optional<DataEdit> GetNextDataEdit(
      IoStatementState &, int maxRepeat = 1) {
    return format_.GetNextDataEdit(*this, maxRepeat);
  }

private:
  MutableModes mutableModes_;
  FormatControl<ChildFormattedIoStatementState> format_;
};

template <Direction DIR>
class ChildListIoStatementState : public ChildIoStatementState<DIR>,
                                  public ListDirectedStatementState<DIR> {
public:
  using ChildIoStatementState<DIR>::ChildIoStatementState;
  using ListDirectedStatementState<DIR>::GetNextDataEdit;
};

template <Direction DIR>
class ChildUnformattedIoStatementState : public ChildIoStatementState<DIR> {
public:
  using ChildIoStatementState<DIR>::ChildIoStatementState;
  bool Receive(char *, std::size_t, std::size_t elementBytes = 0);
};

// OPEN
class OpenStatementState : public ExternalIoStatementBase {
public:
  OpenStatementState(ExternalFileUnit &unit, bool wasExtant,
      const char *sourceFile = nullptr, int sourceLine = 0)
      : ExternalIoStatementBase{unit, sourceFile, sourceLine}, wasExtant_{
                                                                   wasExtant} {}
  bool wasExtant() const { return wasExtant_; }
  void set_status(OpenStatus status) { status_ = status; } // STATUS=
  void set_path(const char *, std::size_t); // FILE=
  void set_position(Position position) { position_ = position; } // POSITION=
  void set_action(Action action) { action_ = action; } // ACTION=
  void set_convert(Convert convert) { convert_ = convert; } // CONVERT=
  void set_access(Access access) { access_ = access; } // ACCESS=
  void set_isUnformatted(bool yes = true) { isUnformatted_ = yes; } // FORM=

  void CompleteOperation();
  int EndIoStatement();

private:
  bool wasExtant_;
  std::optional<OpenStatus> status_;
  std::optional<Position> position_;
  std::optional<Action> action_;
  Convert convert_{Convert::Native};
  OwningPtr<char> path_;
  std::size_t pathLength_;
  std::optional<bool> isUnformatted_;
  std::optional<Access> access_;
};

class CloseStatementState : public ExternalIoStatementBase {
public:
  CloseStatementState(ExternalFileUnit &unit, const char *sourceFile = nullptr,
      int sourceLine = 0)
      : ExternalIoStatementBase{unit, sourceFile, sourceLine} {}
  void set_status(CloseStatus status) { status_ = status; }
  int EndIoStatement();

private:
  CloseStatus status_{CloseStatus::Keep};
};

// For CLOSE(bad unit), WAIT(bad unit, ID=nonzero), INQUIRE(unconnected unit),
// and recoverable BACKSPACE(bad unit)
class NoUnitIoStatementState : public IoStatementBase {
public:
  IoStatementState &ioStatementState() { return ioStatementState_; }
  MutableModes &mutableModes() { return connection_.modes; }
  ConnectionState &GetConnectionState() { return connection_; }
  int badUnitNumber() const { return badUnitNumber_; }
  void CompleteOperation();
  int EndIoStatement();

protected:
  template <typename A>
  NoUnitIoStatementState(A &stmt, const char *sourceFile = nullptr,
      int sourceLine = 0, int badUnitNumber = -1)
      : IoStatementBase{sourceFile, sourceLine}, ioStatementState_{stmt},
        badUnitNumber_{badUnitNumber} {}

private:
  IoStatementState ioStatementState_; // points to *this
  ConnectionState connection_;
  int badUnitNumber_;
};

class NoopStatementState : public NoUnitIoStatementState {
public:
  NoopStatementState(
      const char *sourceFile = nullptr, int sourceLine = 0, int unitNumber = -1)
      : NoUnitIoStatementState{*this, sourceFile, sourceLine, unitNumber} {}
  void set_status(CloseStatus) {} // discards
};

extern template class InternalIoStatementState<Direction::Output>;
extern template class InternalIoStatementState<Direction::Input>;
extern template class InternalFormattedIoStatementState<Direction::Output>;
extern template class InternalFormattedIoStatementState<Direction::Input>;
extern template class InternalListIoStatementState<Direction::Output>;
extern template class InternalListIoStatementState<Direction::Input>;
extern template class ExternalIoStatementState<Direction::Output>;
extern template class ExternalIoStatementState<Direction::Input>;
extern template class ExternalFormattedIoStatementState<Direction::Output>;
extern template class ExternalFormattedIoStatementState<Direction::Input>;
extern template class ExternalListIoStatementState<Direction::Output>;
extern template class ExternalListIoStatementState<Direction::Input>;
extern template class ExternalUnformattedIoStatementState<Direction::Output>;
extern template class ExternalUnformattedIoStatementState<Direction::Input>;
extern template class ChildIoStatementState<Direction::Output>;
extern template class ChildIoStatementState<Direction::Input>;
extern template class ChildFormattedIoStatementState<Direction::Output>;
extern template class ChildFormattedIoStatementState<Direction::Input>;
extern template class ChildListIoStatementState<Direction::Output>;
extern template class ChildListIoStatementState<Direction::Input>;
extern template class ChildUnformattedIoStatementState<Direction::Output>;
extern template class ChildUnformattedIoStatementState<Direction::Input>;

extern template class FormatControl<
    InternalFormattedIoStatementState<Direction::Output>>;
extern template class FormatControl<
    InternalFormattedIoStatementState<Direction::Input>>;
extern template class FormatControl<
    ExternalFormattedIoStatementState<Direction::Output>>;
extern template class FormatControl<
    ExternalFormattedIoStatementState<Direction::Input>>;
extern template class FormatControl<
    ChildFormattedIoStatementState<Direction::Output>>;
extern template class FormatControl<
    ChildFormattedIoStatementState<Direction::Input>>;

class InquireUnitState : public ExternalIoStatementBase {
public:
  InquireUnitState(ExternalFileUnit &unit, const char *sourceFile = nullptr,
      int sourceLine = 0);
  bool Inquire(InquiryKeywordHash, char *, std::size_t);
  bool Inquire(InquiryKeywordHash, bool &);
  bool Inquire(InquiryKeywordHash, std::int64_t, bool &);
  bool Inquire(InquiryKeywordHash, std::int64_t &);
};

class InquireNoUnitState : public NoUnitIoStatementState {
public:
  InquireNoUnitState(const char *sourceFile = nullptr, int sourceLine = 0,
      int badUnitNumber = -1);
  bool Inquire(InquiryKeywordHash, char *, std::size_t);
  bool Inquire(InquiryKeywordHash, bool &);
  bool Inquire(InquiryKeywordHash, std::int64_t, bool &);
  bool Inquire(InquiryKeywordHash, std::int64_t &);
};

class InquireUnconnectedFileState : public NoUnitIoStatementState {
public:
  InquireUnconnectedFileState(OwningPtr<char> &&path,
      const char *sourceFile = nullptr, int sourceLine = 0);
  bool Inquire(InquiryKeywordHash, char *, std::size_t);
  bool Inquire(InquiryKeywordHash, bool &);
  bool Inquire(InquiryKeywordHash, std::int64_t, bool &);
  bool Inquire(InquiryKeywordHash, std::int64_t &);

private:
  OwningPtr<char> path_; // trimmed and NUL terminated
};

class InquireIOLengthState : public NoUnitIoStatementState,
                             public OutputStatementState {
public:
  InquireIOLengthState(const char *sourceFile = nullptr, int sourceLine = 0);
  std::size_t bytes() const { return bytes_; }
  bool Emit(const char *, std::size_t bytes, std::size_t elementBytes = 0);

private:
  std::size_t bytes_{0};
};

class ExternalMiscIoStatementState : public ExternalIoStatementBase {
public:
  enum Which { Flush, Backspace, Endfile, Rewind, Wait };
  ExternalMiscIoStatementState(ExternalFileUnit &unit, Which which,
      const char *sourceFile = nullptr, int sourceLine = 0)
      : ExternalIoStatementBase{unit, sourceFile, sourceLine}, which_{which} {}
  void CompleteOperation();
  int EndIoStatement();

private:
  Which which_;
};

class ErroneousIoStatementState : public IoStatementBase {
public:
  explicit ErroneousIoStatementState(Iostat iostat,
      ExternalFileUnit *unit = nullptr, const char *sourceFile = nullptr,
      int sourceLine = 0)
      : IoStatementBase{sourceFile, sourceLine}, unit_{unit} {
    SetPendingError(iostat);
  }
  int EndIoStatement();
  ConnectionState &GetConnectionState() { return connection_; }
  MutableModes &mutableModes() { return connection_.modes; }

private:
  ConnectionState connection_;
  ExternalFileUnit *unit_{nullptr};
};

} // namespace Fortran::runtime::io
#endif // FORTRAN_RUNTIME_IO_STMT_H_
