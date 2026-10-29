// Fixture driver for NativeModuleRestoreMatch.hpp: reads one case from stdin and prints the result as JSON.
//   R <type> <xMin> <yMin> <xMax> <yMax>          a recorded (deleted) element, in item order
//   P <guid> <type> <xMin> <yMin> <xMax> <yMax>   a placed element, in placement order
//   T <tolerance>                                  optional
#include "NativeModuleRestoreMatch.hpp"

#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

int main ()
{
	using namespace NativeModuleRestoreMatch;
	std::vector<Recorded> recorded;
	std::vector<Placed> placed;
	double tolerance = Tolerance;
	std::string line;
	while (std::getline (std::cin, line)) {
		std::istringstream in (line);
		std::string kind;
		if (!(in >> kind)) continue;
		if (kind == "R") {
			Recorded item;
			in >> item.type >> item.bounds.xMin >> item.bounds.yMin >> item.bounds.xMax >> item.bounds.yMax;
			recorded.push_back (item);
		} else if (kind == "P") {
			Placed element;
			in >> element.guid >> element.type >> element.bounds.xMin >> element.bounds.yMin >> element.bounds.xMax >> element.bounds.yMax;
			placed.push_back (element);
		} else if (kind == "T") {
			in >> tolerance;
		}
		if (in.fail ()) {
			std::cerr << "bad line: " << line << '\n';
			return 2;
		}
	}
	const Result result = Match (recorded, placed, tolerance);
	std::printf ("{\"ok\":%s,\"code\":\"%s\",\"dx\":%.12g,\"dy\":%.12g,\"ambiguousIdentity\":%s,\"candidates\":%zu,\"maxDeviation\":%.12g,\"placed\":[",
		result.ok ? "true" : "false", result.code.c_str (), result.dx, result.dy, result.ambiguousIdentity ? "true" : "false",
		result.candidates, result.maxDeviation);
	for (std::size_t index = 0; index < result.placedIndex.size (); ++index)
		std::printf ("%s\"%s\"", index ? "," : "", placed[result.placedIndex[index]].guid.c_str ());
	std::printf ("]}\n");
	return 0;
}
