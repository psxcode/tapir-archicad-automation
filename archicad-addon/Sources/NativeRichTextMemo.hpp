#pragma once

#include "CommandBase.hpp"

// Archicad's text memo is deliberately kept behind this neutral bridge.  The
// bridge knows about API_ParagraphType/API_RunType and Unicode memo masks, but
// it does not know Markdown, language policy, AzLat or a preferred font.
namespace NativeRichTextMemo {

constexpr const char* ContractVersion = "native-rich-text-v1";

// Build a Unicode API_ElementMemo from the versioned ObjectState DTO.  The
// caller owns the returned memo handles and must call
// ACAPI_DisposeElemMemoHdls when finished, including after a failed build;
// errorMessage contains a deterministic diagnostic on failure.
GSErrCode BuildMemo (
    const GS::ObjectState& richText,
    API_ElementMemo& memo,
    API_TextType& textData,
    GS::UniString& errorMessage);

// Read the Unicode text payload with a bounded, handle-size-aware path on
// AC25.  The caller remains responsible for checking the GetMemo error and
// disposing the memo handles.  Keeping this helper public lets the legacy
// plain-content projection use exactly the same safety boundary as the rich
// text serializer.
bool ReadUnicodeContent (
    const API_ElementMemo& memo,
    GS::UniString& content,
    GS::UniString& errorMessage);

// Serialize a Unicode text memo into the versioned DTO and also add the
// backwards-compatible plain `content` field to target.  A false result means
// the requested memo was unavailable or exceeded the bounded read contract.
bool AddRichText (
    GS::ObjectState& target,
    const API_ElementMemo& memo,
    GSErrCode memoError,
    const API_TextType* textData,
    GS::UniString& errorMessage);

} // namespace NativeRichTextMemo
