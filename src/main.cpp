// EAR's command line. A stub until milestone 1's composition root (slice S6) boots the graph
// from --config: it parses the options, prints them for --help and otherwise says the graph
// is not built yet.

#include <iostream>
#include <string>

#include <boost/program_options.hpp>

namespace po = boost::program_options;

int main(int argc, char** argv) {
	po::options_description options("Options");
	options.add_options()
		("help,h", "print these options and exit")
		("config", po::value<std::string>()->default_value("ear.json"), "graph file path");

	po::variables_map arguments;
	try {
		po::store(po::parse_command_line(argc, argv, options), arguments);
		po::notify(arguments);
	} catch (const po::error& error) {
		std::cerr << "ear: " << error.what() << "\n\n" << options << '\n';
		return 1;
	}

	if (arguments.count("help")) {
		std::cout << options << '\n';
		return 0;
	}

	std::cerr << "ear: running a graph (" << arguments["config"].as<std::string>()
			  << ") is not built yet\n";
	return 1;
}
