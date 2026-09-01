#include "NativeRichTextMemo.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <new>
#include <vector>

namespace NativeRichTextMemo {

namespace {

constexpr GSSize MaxContentUnits = 1'000'000;
constexpr GSSize MaxParagraphs = 4'096;
constexpr GSSize MaxRunsPerParagraph = 4'096;
constexpr GSSize MaxTabsPerParagraph = 256;
constexpr GSSize MaxEolPositionsPerParagraph = 4'096;

struct RunSpec {
    Int32 from = 0;
    Int32 range = 0;
    short pen = 0;
    unsigned short faceBits = 0;
    short font = 0;
    unsigned short effectBits = 0;
    double size = 0.0;
};

struct TabSpec {
    API_TabID type = APITab_Left;
    double position = 0.0;
};

struct ParagraphSpec {
    Int32 from = 0;
    Int32 range = 0;
    API_JustID just = APIJust_Left;
    double firstIndent = 0.0;
    double indent = 0.0;
    double rightIndent = 0.0;
    double spacing = -1.0;
    std::vector<TabSpec> tabs;
    std::vector<RunSpec> runs;
    std::vector<Int32> eolPositions;
};

bool IsFinite (const double value)
{
    return std::isfinite (value);
}

GSErrCode Fail (GS::UniString& errorMessage, const GS::UniString& message, const GSErrCode code = APIERR_BADPARS)
{
    errorMessage = message;
    return code;
}

bool GetInt32 (const GS::ObjectState& object, const char* name, Int32& value)
{
    return object.Get (name, value);
}

bool GetFinite (const GS::ObjectState& object, const char* name, double& value)
{
    return object.Get (name, value) && IsFinite (value);
}

GSErrCode ParseRun (
    const GS::ObjectState& state,
    const Int32 paragraphRange,
    RunSpec& run,
    GS::UniString& errorMessage)
{
    if (!GetInt32 (state, "from", run.from) || !GetInt32 (state, "range", run.range) ||
        run.from < 0 || run.range < 0 || run.from > paragraphRange - run.range) {
        return Fail (errorMessage, "Rich Text run has an invalid range.");
    }

    Int32 pen = 0;
    Int32 font = 0;
    Int32 faceBits = 0;
    Int32 effectBits = 0;
    if (!GetInt32 (state, "pen", pen) || !GetInt32 (state, "fontIndex", font) ||
        !GetInt32 (state, "faceBits", faceBits) || !GetInt32 (state, "effectBits", effectBits) ||
        pen < std::numeric_limits<short>::min () || pen > std::numeric_limits<short>::max () ||
        font < std::numeric_limits<short>::min () || font > std::numeric_limits<short>::max () ||
        faceBits < 0 || faceBits > std::numeric_limits<unsigned short>::max () ||
        effectBits < 0 || effectBits > std::numeric_limits<unsigned short>::max () ||
        !GetFinite (state, "sizeMm", run.size)) {
        return Fail (errorMessage, "Rich Text run has invalid style values.");
    }

    run.pen = static_cast<short> (pen);
    run.font = static_cast<short> (font);
    run.faceBits = static_cast<unsigned short> (faceBits);
    run.effectBits = static_cast<unsigned short> (effectBits);
    return NoError;
}

GSErrCode ParseParagraph (
    const GS::ObjectState& state,
    const GS::UniString& content,
    const GSSize contentLength,
    const GSSize previousEnd,
    ParagraphSpec& paragraph,
    GS::UniString& errorMessage)
{
    if (!GetInt32 (state, "from", paragraph.from) || !GetInt32 (state, "range", paragraph.range) ||
        paragraph.from < 0 || paragraph.range < 0 ||
        static_cast<GSSize> (paragraph.from) > contentLength ||
        static_cast<GSSize> (paragraph.range) > contentLength - static_cast<GSSize> (paragraph.from) ||
        static_cast<GSSize> (paragraph.from) < previousEnd) {
        return Fail (errorMessage, "Rich Text paragraph has an invalid range.");
    }

    Int32 justification = 0;
    if (!GetInt32 (state, "justification", justification) ||
        justification < static_cast<Int32> (APIJust_Left) ||
        justification > static_cast<Int32> (APIJust_Full) ||
        !GetFinite (state, "firstIndentMm", paragraph.firstIndent) ||
        !GetFinite (state, "indentMm", paragraph.indent) ||
        !GetFinite (state, "rightIndentMm", paragraph.rightIndent) ||
        !GetFinite (state, "spacing", paragraph.spacing)) {
        return Fail (errorMessage, "Rich Text paragraph has invalid layout values.");
    }
    paragraph.just = static_cast<API_JustID> (justification);

    GS::Array<GS::ObjectState> tabs;
    if (!state.Get ("tabs", tabs) || tabs.GetSize () > MaxTabsPerParagraph) {
        return Fail (errorMessage, "Rich Text paragraph tabs are missing or too numerous.");
    }
    for (const GS::ObjectState& tabState : tabs) {
        Int32 type = 0;
        double position = 0.0;
        if (!GetInt32 (tabState, "type", type) || type < static_cast<Int32> (APITab_Left) ||
            type > static_cast<Int32> (APITab_Decimalpoint) || !GetFinite (tabState, "positionMm", position)) {
            return Fail (errorMessage, "Rich Text paragraph contains an invalid tab.");
        }
        paragraph.tabs.push_back ({ static_cast<API_TabID> (type), position });
    }

    GS::Array<Int32> eolPositions;
    if (!state.Get ("eolPositions", eolPositions) || eolPositions.GetSize () > MaxEolPositionsPerParagraph) {
        return Fail (errorMessage, "Rich Text EOL positions are missing or too numerous.");
    }
    Int32 previousEol = -1;
    for (const Int32 position : eolPositions) {
        if (position < 0 || position <= previousEol || paragraph.range == 0 || position >= paragraph.range) {
            return Fail (errorMessage, "Rich Text EOL positions are not ordered or are outside the paragraph.");
        }
        previousEol = position;
        paragraph.eolPositions.push_back (position);
    }

    // Runs never contain line-end characters.  API_ParagraphType ranges also
    // cannot contain CR/LF (APIERR_PARIMPLICIT); physical paragraph separators
    // are the gaps between adjacent paragraph records.  Empty paragraphs
    // still carry one zero-length run, matching the DevKit convention.
    GS::Array<GS::ObjectState> runs;
    if (!state.Get ("runs", runs) || runs.IsEmpty () || runs.GetSize () > MaxRunsPerParagraph) {
        return Fail (errorMessage, "Rich Text paragraph must contain at least one run.");
    }
    const GS::uchar_t* contentUnits = content.ToUStr ();
    for (Int32 offset = 0; offset < paragraph.range; ++offset) {
        const GS::uchar_t character = contentUnits[paragraph.from + offset];
        if (character == '\n' || character == '\r')
            return Fail (errorMessage, "Rich Text paragraph range must not contain a line-end character.", APIERR_PARIMPLICIT);
    }

    Int32 previousRunEnd = 0;
    for (const GS::ObjectState& runState : runs) {
        RunSpec run;
        const GSErrCode runError = ParseRun (runState, paragraph.range, run, errorMessage);
        if (runError != NoError) return runError;
        if (run.from < previousRunEnd) return Fail (errorMessage, "Rich Text runs overlap.");
        if (run.from > previousRunEnd) return Fail (errorMessage, "Rich Text runs must cover the paragraph without gaps.");
        previousRunEnd = run.from + run.range;
        paragraph.runs.push_back (run);
    }
    if (previousRunEnd < paragraph.range)
        return Fail (errorMessage, "Rich Text runs must cover the complete paragraph range.");

    return NoError;
}

bool AddUnicodeContent (API_ElementMemo& memo, const GS::UniString& content, GS::UniString& errorMessage)
{
    if (content.GetLength () > MaxContentUnits ||
        content.GetLength () >= static_cast<USize> (std::numeric_limits<Int32>::max ())) {
        errorMessage = "Rich Text content is too large.";
        return false;
    }
#ifdef ServerMainVers_2800
    std::unique_ptr<GS::UniString> replacement (new (std::nothrow) GS::UniString { content });
    if (replacement == nullptr) {
        errorMessage = "Failed to allocate Rich Text content.";
        return false;
    }
    memo.textContent = replacement.release ();
#else
    const GSSize units = static_cast<GSSize> (content.GetLength ()) + 1;
    if (units > std::numeric_limits<GSSize>::max () / sizeof (GS::uchar_t)) {
        errorMessage = "Rich Text content size overflows the memo allocation.";
        return false;
    }
    GSHandle handle = BMhAllClear (units * sizeof (GS::uchar_t));
    if (handle == nullptr || *handle == nullptr) {
        if (handle != nullptr) BMKillHandle (&handle);
        errorMessage = "Failed to allocate Rich Text content.";
        return false;
    }
    memo.textContent = reinterpret_cast<char**> (handle);
    GS::ucsncpy (reinterpret_cast<GS::uchar_t*> (*memo.textContent), content.ToUStr (), static_cast<USize> (units));
    reinterpret_cast<GS::uchar_t*> (*memo.textContent)[units - 1] = 0;
#endif
    return true;
}

void AddParagraphLayoutDefaults (GS::ObjectState& paragraphState, const API_TextType& textData)
{
    paragraphState.Add ("justification", static_cast<Int32> (textData.just));
    paragraphState.Add ("firstIndentMm", 0.0);
    paragraphState.Add ("indentMm", 0.0);
    paragraphState.Add ("rightIndentMm", 0.0);
    paragraphState.Add ("spacing", -1.0);
    paragraphState.AddList<GS::ObjectState> ("tabs");
}

bool AddPlainTextParagraphs (
    GS::ObjectState& richText,
    const GS::UniString& content,
    const API_TextType& textData,
    GS::UniString& errorMessage)
{
    const auto& paragraphList = richText.AddList<GS::ObjectState> ("paragraphs");
    const GS::uchar_t* units = content.ToUStr ();
    const GSSize length = static_cast<GSSize> (content.GetLength ());
    GSSize paragraphFrom = 0;
    GSSize paragraphCount = 0;

    const auto appendParagraph = [&] (const GSSize from, const GSSize range) -> bool {
        if (paragraphCount >= MaxParagraphs) {
            errorMessage = "The Archicad text memo has too many fallback paragraphs.";
            return false;
        }
        GS::ObjectState paragraph;
        paragraph.Add ("from", static_cast<Int32> (from));
        paragraph.Add ("range", static_cast<Int32> (range));
        AddParagraphLayoutDefaults (paragraph, textData);
        const auto& runs = paragraph.AddList<GS::ObjectState> ("runs");
        runs (GS::ObjectState (
            "from", 0,
            "range", static_cast<Int32> (range),
            "pen", static_cast<Int32> (textData.pen),
            "fontIndex", static_cast<Int32> (textData.font),
            "sizeMm", textData.size,
            "faceBits", static_cast<Int32> (textData.faceBits),
            "effectBits", 0));
        const auto& eolPositions = paragraph.AddList<Int32> ("eolPositions");
        if (range > 0)
            eolPositions (static_cast<Int32> (range - 1));
        paragraphList (paragraph);
        ++paragraphCount;
        return true;
    };

    GSSize cursor = 0;
    while (cursor <= length) {
        GSSize lineEnd = cursor;
        while (lineEnd < length && units[lineEnd] != '\n' && units[lineEnd] != '\r') ++lineEnd;
        if (!appendParagraph (paragraphFrom, lineEnd - cursor)) return false;
        if (lineEnd == length) break;
        if (units[lineEnd] == '\r' && lineEnd + 1 < length && units[lineEnd + 1] == '\n')
            cursor = lineEnd + 2;
        else
            cursor = lineEnd + 1;
        paragraphFrom = cursor;
    }
    return true;
}

bool AllocateParagraphs (API_ElementMemo& memo, const std::vector<ParagraphSpec>& paragraphs, GS::UniString& errorMessage)
{
    const GSSize paragraphCount = static_cast<GSSize> (paragraphs.size ());
    if (paragraphCount == 0 || paragraphCount > MaxParagraphs ||
        paragraphCount > std::numeric_limits<GSSize>::max () / sizeof (API_ParagraphType)) {
        errorMessage = "Rich Text paragraph count is outside the supported range.";
        return false;
    }
    memo.paragraphs = reinterpret_cast<API_ParagraphType**> (
        BMhAllClear (paragraphCount * sizeof (API_ParagraphType)));
    if (memo.paragraphs == nullptr || *memo.paragraphs == nullptr) {
        errorMessage = "Failed to allocate Rich Text paragraphs.";
        return false;
    }

    for (GSSize index = 0; index < paragraphCount; ++index) {
        const ParagraphSpec& source = paragraphs[index];
        API_ParagraphType& target = (*memo.paragraphs)[index];
        target.from = source.from;
        target.range = source.range;
        target.just = source.just;
        target.firstIndent = source.firstIndent;
        target.indent = source.indent;
        target.rightIndent = source.rightIndent;
        target.spacing = source.spacing;

        // The DevKit examples allocate one tab and one run even when the
        // logical DTO contains no tabs or an empty paragraph.  Keeping that
        // convention avoids null handles in AC25 while preserving the DTO's
        // empty arrays on read.
        const GSSize tabCount = std::max<GSSize> (1, static_cast<GSSize> (source.tabs.size ()));
        const GSSize runCount = std::max<GSSize> (1, static_cast<GSSize> (source.runs.size ()));
        const GSSize eolCount = static_cast<GSSize> (source.eolPositions.size ());
        if (tabCount > std::numeric_limits<GSSize>::max () / sizeof (API_TabType) ||
            runCount > std::numeric_limits<GSSize>::max () / sizeof (API_RunType) ||
            eolCount > std::numeric_limits<GSSize>::max () / sizeof (Int32)) {
            errorMessage = "Rich Text paragraph allocation would overflow.";
            return false;
        }
        target.tab = reinterpret_cast<API_TabType*> (
            BMAllocatePtr (tabCount * sizeof (API_TabType), ALLOCATE_CLEAR, 0));
        target.run = reinterpret_cast<API_RunType*> (
            BMAllocatePtr (runCount * sizeof (API_RunType), ALLOCATE_CLEAR, 0));
        if (target.tab == nullptr || target.run == nullptr) {
            errorMessage = "Failed to allocate Rich Text paragraph styles.";
            return false;
        }
        for (GSSize tabIndex = 0; tabIndex < static_cast<GSSize> (source.tabs.size ()); ++tabIndex) {
            target.tab[tabIndex].type = source.tabs[tabIndex].type;
            target.tab[tabIndex].pos = source.tabs[tabIndex].position;
        }
        for (GSSize runIndex = 0; runIndex < static_cast<GSSize> (source.runs.size ()); ++runIndex) {
            const RunSpec& sourceRun = source.runs[runIndex];
            API_RunType& targetRun = target.run[runIndex];
            targetRun.from = sourceRun.from;
            targetRun.range = sourceRun.range;
            targetRun.pen = sourceRun.pen;
            targetRun.faceBits = sourceRun.faceBits;
            targetRun.font = sourceRun.font;
            targetRun.effectBits = sourceRun.effectBits;
            targetRun.size = sourceRun.size;
        }
        if (!source.eolPositions.empty ()) {
            target.eolPos = reinterpret_cast<Int32*> (
                BMAllocatePtr (eolCount * sizeof (Int32), ALLOCATE_CLEAR, 0));
            if (target.eolPos == nullptr) {
                errorMessage = "Failed to allocate Rich Text EOL positions.";
                return false;
            }
            for (GSSize eolIndex = 0; eolIndex < eolCount; ++eolIndex)
                target.eolPos[eolIndex] = source.eolPositions[eolIndex];
        }
    }
    return true;
}

} // namespace

GSErrCode BuildMemo (
    const GS::ObjectState& richText,
    API_ElementMemo& memo,
    API_TextType& textData,
    GS::UniString& errorMessage)
{
    GS::UniString schemaVersion;
    GS::UniString encoding;
    GS::UniString content;
    if (!richText.Get ("schemaVersion", schemaVersion) || schemaVersion != ContractVersion)
        return Fail (errorMessage, "Unsupported native Rich Text schema version.");
    if (!richText.Get ("encoding", encoding) || encoding != "unicode")
        return Fail (errorMessage, "Native Rich Text must use Unicode encoding.");
    if (!richText.Get ("content", content))
        return Fail (errorMessage, "Native Rich Text is missing content.");

    const GS::ObjectState* offsets = richText.Get ("offsets");
    GS::UniString unit;
    GS::UniString paragraphSpace;
    GS::UniString runSpace;
    GS::UniString eolSpace;
    if (offsets == nullptr || !offsets->Get ("unit", unit) || !offsets->Get ("paragraph", paragraphSpace) ||
        !offsets->Get ("run", runSpace) || !offsets->Get ("eol", eolSpace) ||
        unit != "archicad-unicode-character" || paragraphSpace != "content" ||
        runSpace != "paragraph" || eolSpace != "paragraph") {
        return Fail (errorMessage, "Native Rich Text offset spaces are invalid.");
    }

    GS::Array<GS::ObjectState> paragraphStates;
    if (!richText.Get ("paragraphs", paragraphStates) || paragraphStates.IsEmpty () ||
        paragraphStates.GetSize () > MaxParagraphs) {
        return Fail (errorMessage, "Native Rich Text must contain a bounded paragraph array.");
    }

    std::vector<ParagraphSpec> paragraphs;
    paragraphs.reserve (paragraphStates.GetSize ());
    GSSize previousEnd = 0;
    for (const GS::ObjectState& paragraphState : paragraphStates) {
        ParagraphSpec paragraph;
        const GSErrCode paragraphError = ParseParagraph (
            paragraphState, content, content.GetLength (), previousEnd, paragraph, errorMessage);
        if (paragraphError != NoError) return paragraphError;
        const GSSize gap = static_cast<GSSize> (paragraph.from) - previousEnd;
        if (paragraphs.empty () && paragraph.from != 0) {
            return Fail (errorMessage, "Native Rich Text first paragraph must start at content offset 0.");
        }
        if (paragraphs.size () > 0 && gap == 0) {
            return Fail (errorMessage, "Native Rich Text paragraph ranges require a line-end separator.");
        }
        if (paragraphs.size () > 0 && gap > 0) {
            const GS::uchar_t* contentUnits = content.ToUStr ();
            const bool isLineEnd = (gap == 1 && (contentUnits[previousEnd] == '\n' || contentUnits[previousEnd] == '\r'))
                || (gap == 2 && contentUnits[previousEnd] == '\r' && contentUnits[previousEnd + 1] == '\n');
            if (!isLineEnd) {
                return Fail (errorMessage, "Native Rich Text paragraph gap must be one line-end sequence.");
            }
        }
        previousEnd = static_cast<GSSize> (paragraph.from + paragraph.range);
        paragraphs.push_back (std::move (paragraph));
    }
    if (static_cast<GSSize> (content.GetLength ()) > previousEnd) {
        const GS::uchar_t* contentUnits = content.ToUStr ();
        const GSSize trailingLength = static_cast<GSSize> (content.GetLength ()) - previousEnd;
        const bool isLineEnd = (trailingLength == 1 && (contentUnits[previousEnd] == '\n' || contentUnits[previousEnd] == '\r'))
            || (trailingLength == 2 && contentUnits[previousEnd] == '\r' && contentUnits[previousEnd + 1] == '\n');
        if (!isLineEnd) {
            return Fail (errorMessage, "Native Rich Text trailing data must be one line-end sequence.");
        }
    }

    // BuildMemo never disposes a partially built memo.  Ownership stays with
    // the caller even on failure, which makes the OnExit guards at call sites
    // the single, unambiguous disposal point.
    if (!AddUnicodeContent (memo, content, errorMessage))
        return APIERR_MEMFULL;
    if (!AllocateParagraphs (memo, paragraphs, errorMessage))
        return APIERR_MEMFULL;

#ifndef ServerMainVers_2800
    textData.charCode = CC_UniCode;
#endif
    // The eolPos array owns explicit line boundaries.  Match the DevKit
    // multistyle example and leave wrapping enabled for lines not listed by
    // the caller.
    textData.nonBreaking = false;
    textData.multiStyle = true;
    textData.useEolPos = true;
    GSSize lineCount = 0;
    for (const ParagraphSpec& paragraph : paragraphs) {
        const GSSize eolCount = static_cast<GSSize> (paragraph.eolPositions.size ());
        const GSSize paragraphLines = std::max<GSSize> (static_cast<GSSize> (1), eolCount);
        if (lineCount > static_cast<GSSize> (std::numeric_limits<Int32>::max ()) - paragraphLines) {
            return Fail (errorMessage, "Rich Text line count exceeds the supported range.");
        }
        lineCount += paragraphLines;
    }
    textData.nLine = static_cast<Int32> (lineCount);
    return NoError;
}

bool ReadUnicodeContent (
    const API_ElementMemo& memo,
    GS::UniString& content,
    GS::UniString& errorMessage)
{
    if (memo.textContent == nullptr) {
        errorMessage = "The Archicad Unicode text memo was empty.";
        return false;
    }

#ifdef ServerMainVers_2800
    const GS::UniString* text = memo.textContent;
    const USize length = text->GetLength ();
    if (length > static_cast<USize> (MaxContentUnits)) {
        errorMessage = "The Archicad text memo exceeded the bounded read limit.";
        return false;
    }
    content = *text;
#else
    if (*memo.textContent == nullptr) {
        errorMessage = "The Archicad Unicode text memo was empty.";
        return false;
    }

    // AC25 stores Unicode text in a byte-sized BM handle containing
    // GS::uchar_t units.  Inspect the handle size before dereferencing the
    // payload; a malformed/unterminated memo must never reach a scanning
    // GS::UniString constructor.
    const GSHandle handle = reinterpret_cast<GSHandle> (memo.textContent);
    const GSSize bytes = BMhGetSize (handle);
    if (bytes == 0 || bytes % sizeof (GS::uchar_t) != 0 ||
        bytes > (MaxContentUnits + 1) * sizeof (GS::uchar_t)) {
        errorMessage = "The Archicad Unicode text memo has an invalid bounded handle size.";
        return false;
    }

    const GSSize units = bytes / sizeof (GS::uchar_t);
    const GS::uchar_t* text = reinterpret_cast<const GS::uchar_t*> (*memo.textContent);
    GSSize length = 0;
    while (length < units && text[length] != 0)
        ++length;
    if (length == units) {
        errorMessage = "The Archicad Unicode text memo is not NUL terminated within its handle.";
        return false;
    }
    if (length > MaxContentUnits) {
        errorMessage = "The Archicad text memo exceeded the bounded read limit.";
        return false;
    }
    content = GS::UniString (text, static_cast<USize> (length));
#endif
    return true;
}

bool AddRichText (
    GS::ObjectState& target,
    const API_ElementMemo& memo,
    const GSErrCode memoError,
    const API_TextType* textData,
    GS::UniString& errorMessage)
{
    if (memoError != NoError || memo.textContent == nullptr) {
        errorMessage = "The Archicad text memo was unavailable.";
        return false;
    }

    GS::UniString content;
    if (!ReadUnicodeContent (memo, content, errorMessage))
        return false;

    GS::ObjectState richText;
    richText.Add ("schemaVersion", ContractVersion);
    richText.Add ("encoding", "unicode");
    richText.Add ("offsets", GS::ObjectState (
        "unit", "archicad-unicode-character",
        "paragraph", "content",
        "run", "paragraph",
        "eol", "paragraph"));
    richText.Add ("content", content);

    const bool hasParagraphMemo = memo.paragraphs != nullptr && *memo.paragraphs != nullptr
        && BMhGetSize (reinterpret_cast<GSHandle> (memo.paragraphs)) >= sizeof (API_ParagraphType);
    if (!hasParagraphMemo) {
        // Plain/single-style Text elements may not expose a paragraph memo on
        // every Archicad version.  Keep the native contract useful by
        // projecting the Unicode content into one run per physical line using
        // the element's own default style.  This is still neutral native
        // evidence; Markdown/AzLat/Arial policy remains outside this bridge.
        if (textData == nullptr) {
            errorMessage = "The Archicad text memo has no paragraph handle.";
            return false;
        }
        if (!AddPlainTextParagraphs (richText, content, *textData, errorMessage)) return false;
    } else {
        const GSSize paragraphCount = BMhGetSize (reinterpret_cast<GSHandle> (memo.paragraphs)) /
            sizeof (API_ParagraphType);
        if (paragraphCount == 0 || paragraphCount > MaxParagraphs) {
            errorMessage = "The Archicad text memo has an invalid paragraph handle.";
            return false;
        }

        const auto& paragraphList = richText.AddList<GS::ObjectState> ("paragraphs");
        for (GSSize paragraphIndex = 0; paragraphIndex < paragraphCount; ++paragraphIndex) {
        const API_ParagraphType& paragraph = (*memo.paragraphs)[paragraphIndex];
        GS::ObjectState paragraphState;
        paragraphState.Add ("from", paragraph.from);
        paragraphState.Add ("range", paragraph.range);
        paragraphState.Add ("justification", static_cast<Int32> (paragraph.just));
        paragraphState.Add ("firstIndentMm", paragraph.firstIndent);
        paragraphState.Add ("indentMm", paragraph.indent);
        paragraphState.Add ("rightIndentMm", paragraph.rightIndent);
        paragraphState.Add ("spacing", paragraph.spacing);
        const auto& tabs = paragraphState.AddList<GS::ObjectState> ("tabs");
        if (paragraph.tab != nullptr) {
            const GSSize tabCount = BMGetPtrSize (reinterpret_cast<GSPtr> (paragraph.tab)) / sizeof (API_TabType);
            if (tabCount > MaxTabsPerParagraph) {
                errorMessage = "The Archicad text memo has too many paragraph tabs.";
                return false;
            }
            for (GSSize tabIndex = 0; tabIndex < tabCount; ++tabIndex)
                tabs (GS::ObjectState (
                    "type", static_cast<Int32> (paragraph.tab[tabIndex].type),
                    "positionMm", paragraph.tab[tabIndex].pos));
        }

        const auto& runs = paragraphState.AddList<GS::ObjectState> ("runs");
        if (paragraph.run == nullptr) {
            errorMessage = "The Archicad text memo has no paragraph runs.";
            return false;
        }
        const GSSize runCount = BMGetPtrSize (reinterpret_cast<GSPtr> (paragraph.run)) / sizeof (API_RunType);
        if (runCount == 0 || runCount > MaxRunsPerParagraph) {
            errorMessage = "The Archicad text memo has an invalid run handle.";
            return false;
        }
        for (GSSize runIndex = 0; runIndex < runCount; ++runIndex) {
            const API_RunType& run = paragraph.run[runIndex];
            runs (GS::ObjectState (
                "from", run.from,
                "range", run.range,
                "pen", static_cast<Int32> (run.pen),
                "fontIndex", static_cast<Int32> (run.font),
                "sizeMm", run.size,
                "faceBits", static_cast<Int32> (run.faceBits),
                "effectBits", static_cast<Int32> (run.effectBits)));
        }

        const auto& eolPositions = paragraphState.AddList<Int32> ("eolPositions");
        if (paragraph.eolPos != nullptr) {
            const GSSize eolCount = BMGetPtrSize (reinterpret_cast<GSPtr> (paragraph.eolPos)) / sizeof (Int32);
            if (eolCount > MaxEolPositionsPerParagraph) {
                errorMessage = "The Archicad text memo has too many EOL positions.";
                return false;
            }
            for (GSSize eolIndex = 0; eolIndex < eolCount; ++eolIndex)
                eolPositions (paragraph.eolPos[eolIndex]);
        }
        paragraphState.Add ("widthMm", paragraph.width);
            paragraphState.Add ("heightMm", paragraph.height);
            paragraphList (paragraphState);
        }
    }

    target.Add ("content", content);
    target.Add ("richText", richText);
    target.Add ("richTextStatus", "available");
    errorMessage.Clear ();
    return true;
}

} // namespace NativeRichTextMemo
