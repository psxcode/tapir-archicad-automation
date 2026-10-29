#pragma once

// D-347 stage 3b: calibration and identity of a checkpoint module placed back as plain elements.
// Pure and std-only so a fixture test can run it without the DevKit (Test/NativeModuleRestoreMatch).
//
// A module placed at offset zero lands translated by a per-file constant (V1-068, histrs-ac28-r1/r2). The
// translation is found from the recorded bounds of the deleted elements: every placed element of the first
// item's type and size proposes one; it is accepted when every item then matches exactly one distinct
// placed element of its type at its recorded bounds plus the translation. The placed set is the whole
// footprint of the placement, so it must hold exactly as many elements as there are items. Identity is never
// guessed: an item with more than one equal placed element (coincident copies) refuses the restore.

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace NativeModuleRestoreMatch {

constexpr double Tolerance = 1e-6;

struct Box {
	double xMin = 0.0;
	double yMin = 0.0;
	double xMax = 0.0;
	double yMax = 0.0;
};

struct Recorded {
	std::string type;
	Box bounds;
};

struct Placed {
	std::string guid;
	std::string type;
	Box bounds;
};

struct Result {
	bool ok = false;
	/** MODULE_RESTORE_MISMATCH, MODULE_RESTORE_CALIBRATION_FAILED or MODULE_RESTORE_AMBIGUOUS_IDENTITY when not ok. */
	std::string code;
	std::string message;
	double dx = 0.0;
	double dy = 0.0;
	/** Index into the placed elements, one per recorded item, in item order. */
	std::vector<std::size_t> placedIndex;
	/** Some item had more than one equal placed element (coincident copies). */
	bool ambiguousIdentity = false;
	double maxDeviation = 0.0;
	std::size_t candidates = 0;
};

inline bool Finite (const Box& box)
{
	return std::isfinite (box.xMin) && std::isfinite (box.yMin) && std::isfinite (box.xMax) && std::isfinite (box.yMax)
		&& box.xMin <= box.xMax && box.yMin <= box.yMax;
}

inline double Deviation (const Box& placed, const Box& recorded, double dx, double dy)
{
	const double values[] = {
		std::fabs (placed.xMin - (recorded.xMin + dx)), std::fabs (placed.yMin - (recorded.yMin + dy)),
		std::fabs (placed.xMax - (recorded.xMax + dx)), std::fabs (placed.yMax - (recorded.yMax + dy)),
	};
	double worst = 0.0;
	for (double value : values) worst = value > worst ? value : worst;
	return worst;
}

inline Result Refuse (const char* code, const std::string& message)
{
	Result result;
	result.code = code;
	result.message = message;
	return result;
}

/** One translation and one placed element per item, or a refusal. */
inline Result Match (const std::vector<Recorded>& recorded, const std::vector<Placed>& placed, double tolerance = Tolerance)
{
	if (recorded.empty ()) return Refuse ("MODULE_RESTORE_MISMATCH", "No recorded element to restore.");
	if (placed.size () != recorded.size ())
		return Refuse ("MODULE_RESTORE_MISMATCH", "The placement added " + std::to_string (placed.size ()) + " elements; the checkpoint recorded " + std::to_string (recorded.size ()) + ".");
	for (const Recorded& item : recorded)
		if (!Finite (item.bounds)) return Refuse ("MODULE_RESTORE_MISMATCH", "A recorded element has no finite bounds.");
	for (const Placed& element : placed)
		if (!Finite (element.bounds)) return Refuse ("MODULE_RESTORE_MISMATCH", "The placement added an element without finite bounds (" + element.type + ").");

	// Candidate translations from the first item, distinct within the tolerance.
	const Recorded& first = recorded.front ();
	std::vector<std::pair<double, double>> candidates;
	for (const Placed& element : placed) {
		if (element.type != first.type) continue;
		const double width = element.bounds.xMax - element.bounds.xMin;
		const double height = element.bounds.yMax - element.bounds.yMin;
		if (std::fabs (width - (first.bounds.xMax - first.bounds.xMin)) > tolerance
			|| std::fabs (height - (first.bounds.yMax - first.bounds.yMin)) > tolerance) continue;
		const double dx = element.bounds.xMin - first.bounds.xMin;
		const double dy = element.bounds.yMin - first.bounds.yMin;
		bool known = false;
		for (const auto& candidate : candidates)
			if (std::fabs (candidate.first - dx) <= tolerance && std::fabs (candidate.second - dy) <= tolerance) known = true;
		if (!known) candidates.emplace_back (dx, dy);
	}

	std::vector<Result> accepted;
	for (const auto& candidate : candidates) {
		Result trial;
		trial.ok = true;
		trial.dx = candidate.first;
		trial.dy = candidate.second;
		std::vector<bool> used (placed.size (), false);
		for (const Recorded& item : recorded) {
			std::size_t chosen = placed.size ();
			std::size_t equal = 0;
			for (std::size_t index = 0; index < placed.size (); ++index) {
				if (used[index] || placed[index].type != item.type) continue;
				const double deviation = Deviation (placed[index].bounds, item.bounds, trial.dx, trial.dy);
				if (deviation > tolerance) continue;
				if (equal++ == 0) {
					chosen = index;
					if (deviation > trial.maxDeviation) trial.maxDeviation = deviation;
				}
			}
			if (chosen == placed.size ()) { trial.ok = false; break; }
			if (equal > 1) trial.ambiguousIdentity = true;
			used[chosen] = true;
			trial.placedIndex.push_back (chosen);
		}
		if (trial.ok) accepted.push_back (trial);
	}
	if (accepted.size () != 1) {
		Result refusal = Refuse ("MODULE_RESTORE_CALIBRATION_FAILED", accepted.empty ()
			? "No translation maps every recorded element onto one placed element."
			: "Several translations map every recorded element; the placement is ambiguous.");
		refusal.candidates = candidates.size ();
		return refusal;
	}
	Result result = accepted.front ();
	result.candidates = candidates.size ();
	if (result.ambiguousIdentity) {
		Result refusal = Refuse ("MODULE_RESTORE_AMBIGUOUS_IDENTITY", "Two placed elements are equal for one recorded element; the old-to-new identity is not proven.");
		refusal.candidates = result.candidates;
		refusal.ambiguousIdentity = true;
		return refusal;
	}
	return result;
}

}
