#include "ear/graph_config.hpp"
#include "ear/sources.hpp"
#include "ear/server.hpp"

#include "nlohmann/json.hpp"

#include <boost/program_options.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/uuid/string_generator.hpp>

#include <thread>

int run(const nlohmann::json& config) {
	std::cout << "Server run " << std::endl;

	auto devices = ear::AudioIoDevice::getAllDevices();
	std::shared_ptr<ear::AudioIoDevice> device(nullptr);

	DBG("boolean compare");
	if (config.count("device")) {
		DBG("has device");
		std::string string = config["device"].get<std::string>();

		try {
			boost::uuids::string_generator generator;
			boost::uuids::uuid uuid = generator(string);

			device = ear::AudioIoDevice::getDevice(uuid);
		} catch (const std::runtime_error&) {}

		if (device == nullptr) {
			int index = atoi(string.c_str());
			if (string == std::to_string(index) && index < devices.size()) {
				device = devices[index];
			}
		}
	}

	if (device == nullptr && devices.size() > 0) {
		device = devices[0];
	}

	int longestName = 0;
	for (auto& d : devices) {
		longestName = std::max(longestName, d->getName().length());
	}

	int i = 0;
	for (auto& d : devices) {
		//std::cout << "name length=" << d->getName().length() << std::endl;
		std::cout << ((*d == *device) ? "*" : " ")
			<< std::right << std::setw(3) << (i++) << ") ";
		std::cout << std::left << std::setw(longestName+2) << d->getName();
		std::cout << d->getUuid()
			<< " (" << d->getTypeName() << " | ";
		for (auto i : d->getAvailableSampleRates()) {
			std::cout << i << " ";
		}
		std::cout << "| " << d->getDefaultBufferSize() << ") "
			<< std::endl;
	}

	if (device == nullptr) {
		std::cerr << "No device" << std::endl;
		return 1;
	}

	ear::AudioGraphStandalone graph;
	const std::string graphError = ear::buildGraphFromConfig(graph, device, config);
	if (!graphError.empty()) {
		std::cerr << graphError << std::endl;
		return 1;
	}
/*
	auto whiteNoiseNode1 = graph.addNode(std::make_unique<ear::WhiteNoiseProcessor>());
	auto whiteNoiseNode2 = graph.addNode(std::make_unique<ear::WhiteNoiseProcessor>());

	assert(graph.addConnection({{whiteNoiseNode1, 0}, {outputNode, 0}}));
	assert(graph.addConnection({{whiteNoiseNode1, 0}, {outputNode, 1}}));
	assert(graph.addConnection({{whiteNoiseNode2, 0}, {outputNode, 1}}));

	std::cout << "Connected white noise to output" << std::endl;


	boost::asio::io_context io_context;
	auto airplayProcessor = std::make_unique<ear::AirplayProcessor>("/tmp/airplay", io_context);
	//airplayProcessor->start_reading();
	auto airplayNode = graph.addNode(std::move(airplayProcessor));

	assert(graph.addConnection({{airplayNode, 0}, {outputNode, 0}}));
	assert(graph.addConnection({{airplayNode, 1}, {outputNode, 1}}));

	std::thread t([&io_context] {

		io_context.run();
	});
*/
	bool isOpen = device->open();
	if (!isOpen) {
		std::cerr << "Failed to open device:" << device->getName() << std::endl;
		return 1;
	}

	device->start();

	std::cout << "Server running" << std::endl;

	const int THREADS = 0;
	boost::beast::net::io_context io(THREADS);
	ear::RpcServer server(io, boost::asio::ip::tcp::endpoint(boost::beast::net::ip::make_address("0.0.0.0"), 7777));
	server.run();

	boost::beast::net::signal_set signals(io, SIGINT, SIGTERM);
	signals.async_wait([&] (boost::beast::error_code const&, int) {
		std::cout << "Signal interrupt" << std::endl;
		io.stop();
	});

	std::vector<std::thread> threads;
	for (int i = 0; i < THREADS; ++i) {
		threads.emplace_back([&io] { io.run(); });
	}
	io.run();

	for (auto& thread : threads) {
		thread.join();
	}

	device->close();

	return 0;
}
