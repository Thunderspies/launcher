#ifndef LAUNCHER_TEST_SUPPORT_H
#define LAUNCHER_TEST_SUPPORT_H

#include <launcher/launcher.h>
#include "descriptors.h"
#include <yyjson.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <chrono>
#include <stdexcept>
#include <map>
#include <set>
#include <mutex>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using test_socket = SOCKET;
#define TEST_BAD_SOCKET INVALID_SOCKET
#define test_close_socket closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using test_socket = int;
#define TEST_BAD_SOCKET -1
#define test_close_socket close
#endif

namespace launcher_test {
namespace fs = std::filesystem;

inline std::string read(const fs::path &path)
{
	std::ifstream file(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(file), {});
}

inline void write(const fs::path &path, const std::string &bytes)
{
	fs::create_directories(path.parent_path());
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	file << bytes;
	if (!file)
		throw std::runtime_error("Cannot write fixture " +
			path.string());
}

template<class Edit>
inline void edit_json(const fs::path &path, Edit edit)
{
	std::string bytes = read(path);
	yyjson_doc *doc = yyjson_read(bytes.data(), bytes.size(), 0);
	if (!doc)
		throw std::runtime_error("Invalid JSON fixture " + path.string());
	yyjson_mut_doc *copy = yyjson_doc_mut_copy(doc, nullptr);
	yyjson_doc_free(doc);
	edit(copy, yyjson_mut_doc_get_root(copy));
	size_t size = 0;
	char *saved = yyjson_mut_write(copy, 0, &size);
	write(path, std::string(saved, size));
	free(saved);
	yyjson_mut_doc_free(copy);
}

inline fs::path record_path(const fs::path &root, const std::string &key)
{
	for (const auto &entry : fs::recursive_directory_iterator(root))
		if (entry.path().filename() == key + ".json" &&
			entry.path().parent_path().filename() == "revisions")
			return entry.path();
	throw std::runtime_error("Missing fixture revision " + key);
}

inline std::string record_field(const std::string &bytes, const char *field)
{
	yyjson_doc *doc = yyjson_read(bytes.data(), bytes.size(), 0);
	const char *text = yyjson_get_str(yyjson_obj_get(
		yyjson_doc_get_root(doc), field));
	std::string result = text ? text : "";
	yyjson_doc_free(doc);
	return result;
}

inline std::string publish_record(const fs::path &root,
	const std::string &record)
{
	std::string key = fingerprint(record);
	write(root / "revisions" / (key + ".json"), record);
	return key;
}

inline void publish_launch(const fs::path &channel,
	const std::string &settings)
{
	std::string id = record_field(settings, "release");
	fs::path release = channel.parent_path().parent_path() /
		"releases" / (id + ".json");
	edit_json(release, [&](yyjson_mut_doc *doc, yyjson_mut_val *root) {
		yyjson_doc *source = yyjson_read(settings.data(), settings.size(), 0);
		yyjson_val *metadata = yyjson_doc_get_root(source);
		for (const char *field : {"entry_point", "run_arguments"})
			yyjson_mut_obj_put(root, yyjson_mut_strcpy(doc, field),
				yyjson_val_mut_copy(doc, yyjson_obj_get(metadata, field)));
		yyjson_doc_free(source);
	});
	write(channel, "{\"format\":1,\"release\":\"" + id + "\"}");
}

inline std::string rename_in_manifest(const fs::path &root,
	std::string manifest, const std::string &from, const std::string &to)
{
	std::string old_key;
	std::string record;
	for (const auto &entry : fs::recursive_directory_iterator(root)) {
		if (entry.path().parent_path().filename() != "revisions")
			continue;
		std::string bytes = read(entry.path());
		std::string key = entry.path().stem().string();
		if (record_field(bytes, "name") == from &&
			manifest.find(key) != std::string::npos) {
			old_key = key;
			record = bytes;
			break;
		}
	}
	if (record.empty())
		throw std::runtime_error("Missing named fixture " + from);
	std::string target = "\"name\": \"" + from + "\"";
	size_t offset = record.find(target);
	if (offset == std::string::npos)
		throw std::runtime_error("Noncanonical fixture name");
	record.replace(offset, target.size(), "\"name\": \"" + to + "\"");
	std::string key = publish_record(root, record);
	for (size_t at = 0; (at = manifest.find(old_key, at)) != std::string::npos;)
		manifest.replace(at, old_key.size(), key), at += key.size();
	return manifest;
}

inline void multiple_archives(const fs::path &root, const char *id = "multi")
{
	std::string original = read(root / "releases/r1.json");
	std::string changed = rename_in_manifest(root, original,
		"assets/data.bin", "other/data.bin");
	yyjson_doc *doc = yyjson_read(changed.data(), changed.size(), 0);
	yyjson_val *assets = yyjson_obj_get(yyjson_obj_get(
		yyjson_doc_get_root(doc), "groups"), "assets");
	std::string key = yyjson_get_str(yyjson_arr_get(
		yyjson_obj_get(assets, "files"), 0));
	yyjson_doc_free(doc);
	fs::path target = root / "releases" / (std::string(id) + ".json");
	write(target, original);
	edit_json(target, [&](yyjson_mut_doc *doc, yyjson_mut_val *root) {
		yyjson_mut_obj_put(root, yyjson_mut_str(doc, "release"),
			yyjson_mut_strcpy(doc, id));
		yyjson_mut_val *group = yyjson_mut_obj(doc);
		yyjson_mut_val *files = yyjson_mut_arr(doc);

		yyjson_mut_arr_add_strcpy(doc, files, key.c_str());
		yyjson_mut_obj_add_str(doc, group, "path", "piggs/second.hogg");
		yyjson_mut_obj_add_val(doc, group, "files", files);
		yyjson_mut_obj_add_val(doc, yyjson_mut_obj_get(root, "groups"),
			"second", group);
	});
}

struct HttpServer {
	struct Response {
		int status = 200;
		std::string encoding = "identity";
		std::string body;
		int64_t declared_size = -1;
	};
	std::mutex response_mutex;
	std::map<std::string, Response> responses;
	std::map<std::string, size_t> requests;
	test_socket listener = TEST_BAD_SOCKET;
	std::thread worker;
	std::atomic<bool> stopped{false};
	std::atomic<size_t> object_requests{0};
	std::atomic<size_t> descriptor_requests{0};
	std::atomic<size_t> authenticated_requests{0};
	fs::path root;
	std::string url;

	explicit HttpServer(const fs::path &directory) : root(directory)
	{
#ifdef _WIN32
		WSADATA data;
		if (WSAStartup(MAKEWORD(2, 2), &data))
			throw std::runtime_error(
				"Cannot initialize test sockets");
#endif
		listener = socket(AF_INET, SOCK_STREAM, 0);
		if (listener == TEST_BAD_SOCKET)
			throw std::runtime_error(
				"Cannot create fixture listener");
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		address.sin_port = 0;
		if (bind(listener, (sockaddr *)&address, sizeof(address)) ||
			listen(listener, 16))
			throw std::runtime_error(
				"Cannot bind fixture listener");
#ifdef _WIN32
		int length = sizeof(address);
#else
		socklen_t length = sizeof(address);
#endif
		getsockname(listener, (sockaddr *)&address, &length);
		url = "http://127.0.0.1:" +
			std::to_string(ntohs(address.sin_port)) + "/";
		worker = std::thread([this] { serve(); });
	}

	void set_response(const std::string &path, const Response &response)
	{
		std::lock_guard<std::mutex> lock(response_mutex);
		responses[path] = response;
	}

	size_t request_count(const std::string &path)
	{
		std::lock_guard<std::mutex> lock(response_mutex);
		return requests[path];
	}

	void serve()
	{
		while (!stopped) {
			test_socket client = accept(listener, nullptr, nullptr);
			if (client == TEST_BAD_SOCKET)
				continue;
			std::string request;
			char buffer[4096];
			while (request.find("\r\n\r\n") == std::string::npos &&
				request.size() < 16384) {
				int count = recv(client, buffer,
					sizeof(buffer), 0);
				if (count <= 0)
					break;
				request.append(buffer, count);
			}
			size_t begin = request.find(' ');
			size_t end = request.find(' ', begin + 1);
			std::string path = begin == std::string::npos ? "" :
				request.substr(begin + 2, end - begin - 2);
			if (path.find("/BLOB") != std::string::npos ||
				path.find("/headers/") != std::string::npos)
				++object_requests;
			if (path.rfind("revisions/", 0) == 0)
				++descriptor_requests;
			if (request.find("Authorization:") !=
				std::string::npos ||
				request.find("x-amz-") != std::string::npos)
				++authenticated_requests;
			fs::path file = root / path;
			bool found = path.find("..") == std::string::npos &&
				fs::is_regular_file(file);
			Response selected;
			selected.status = found ? 200 : 404;
			selected.body = found ? read(file) : "missing";
			{
				std::lock_guard<std::mutex> lock(
					response_mutex);
				++requests[path];
				auto override = responses.find(path);

				if (override != responses.end())
					selected = override->second;
			}
			size_t declared = selected.declared_size < 0 ?
				selected.body.size() :
				(size_t)selected.declared_size;
			std::string response = "HTTP/1.1 " +
				std::to_string(selected.status) +
				" Fixture\r\n" +
				"Content-Length: " + std::to_string(declared) +
				"\r\nContent-Encoding: " + selected.encoding +
				"\r\nConnection: close\r\n\r\n" + selected.body;
			size_t sent = 0;
			while (sent < response.size()) {
#ifdef MSG_NOSIGNAL
				int flags = MSG_NOSIGNAL;
#else
				int flags = 0;
#endif
				int count = send(client, response.data() + sent,
					(int)(response.size() - sent), flags);
				if (count <= 0)
					break;
				sent += count;
			}
			test_close_socket(client);
		}
	}

	~HttpServer()
	{
		stopped = true;
#ifdef _WIN32
		shutdown(listener, SD_BOTH);
#else
		shutdown(listener, SHUT_RDWR);
#endif
		test_close_socket(listener);
		if (worker.joinable())
			worker.join();
#ifdef _WIN32
		WSACleanup();
#endif
	}
};

struct Fixture {
	fs::path base;
	fs::path install;
	fs::path public_root;
	HttpServer *server = nullptr;
	launcher_handle *handle = nullptr;
	launcher_error error{};

	Fixture()
	{
		static std::atomic<unsigned> serial{0};
		auto stamp = std::chrono::steady_clock::now()
			.time_since_epoch().count();
		base = fs::temp_directory_path() /
			("launcher-test-" + std::to_string(stamp) + "-" +
			std::to_string(++serial));
		install = base / "game";
		public_root = base / "public";
		fs::create_directories(base);
		fs::copy(fs::path(LAUNCHER_FIXTURE_DIR) / "public", public_root,
			fs::copy_options::recursive | fs::copy_options::overwrite_existing);
		fs::copy(fs::path(LAUNCHER_FIXTURE_DIR) / "file-public",
			public_root, fs::copy_options::recursive |
			fs::copy_options::overwrite_existing);
		server = new HttpServer(public_root);
	}

	launcher_status open()
	{
		return launcher_open(install.string().c_str(), &handle, &error);
	}

	launcher_status configure(const char *channel = "live")
	{
		launcher_config config{};
		config.base_url = server->url.c_str();
		launcher_file_repository files{server->url.c_str(), nullptr};
		config.file_repositories = &files;
		config.file_repository_count = 1;
		config.channel = channel;
		config.game = "game.exe";
		return launcher_configure(handle, &config, &error);
	}

	void pin(const char *release)
	{
		write(public_root / "channels/live.json",
			"{\"format\":1,\"release\":\"" +
			std::string(release) + "\"}\n");
	}

	~Fixture()
	{
		launcher_close(&handle);
		delete server;
		std::error_code ignored;
		fs::remove_all(base, ignored);
	}
};
}

#endif
