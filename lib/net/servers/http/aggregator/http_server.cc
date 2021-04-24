
/**
 * Source file for class UDPServer
 * Please see udp_server.h and server.h for documentaion
 */

#include "statsdcc/net/servers/http/aggregator/http_server.h"

#include <sys/types.h>
#include <dirent.h>

#include <queue>
#include <boost/algorithm/string.hpp>
#include <boost/bind.hpp>

#include "statsdcc/net/wrapper.h"
#include "statsdcc/version.h"
#include "statsdcc/os.h"
#include "statsdcc/status.h"

namespace statsdcc {
namespace net {
namespace servers {
namespace http {
namespace aggregator {

int HttpServer::main_pid;

MHD_Result HttpServer::handle_request(void *cls,
                               struct MHD_Connection *connection,
                               const char *url,
                               const char *method,
                               const char *version,
                               const char *upload_data,
                               size_t *upload_data_size,
                               void **con_cls) {
  struct MHD_Response *response = NULL;
  MHD_Result ret;

  if (!std::strcmp(url, "/")) {
    char home[] = "<html> <h3>Welcome to statsdcc</h3>\
                    Click <a href=\"/proc\">/proc</a> to see threads status \
                    </br> \
                    Click <a href=\"/app\">/app</a> to see application status \
                  </html>";

    response = MHD_create_response_from_buffer(std::strlen(home),
                                               static_cast<void*>(home),
                                               MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(response, "Content-Type", "text/html");

  } else if (!std::strcmp(url, "/proc")) {
    const char *params = MHD_lookup_connection_value(connection,
                                                     MHD_GET_ARGUMENT_KIND,
                                                     "keys");

    std::vector<std::string> keys;
    if (params != NULL) {
      std::string params_str(params);
      boost::split(keys, params_str, boost::is_any_of(","));
    } else {
      keys = {"Pid", "PPid", "State", "VmSize", "VmPeak", "VmSwap",
              "nonvoluntary_ctxt_switches", "voluntary_ctxt_switches",
              "MemTotal", "MemFree", "SwapTotal", "SwapFree", "cpu cores"};
    }

    Json::Value root = HttpServer::get_sys_info(keys);
    root["proc"] = HttpServer::get_proc_status(keys);

    std::string page = root.toStyledString();
    response = MHD_create_response_from_buffer(page.length(),
                                               (void*) page.c_str(),
                                               MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(response, "Content-Type", "application/json");
  } else if (!std::strcmp(url, "/app")) {
    Json::Value root;

    root["statsdcc"]["version"] = __VERSION;
    root["statsdcc"]["build"] = __BUILD;

    root["config"] = ::config->to_json();
    root["status"] = HttpServer::get_app_status();

    std::string page = root.toStyledString();
    response = MHD_create_response_from_buffer(page.length(),
                                               (void*) page.c_str(),
                                               MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(response, "Content-Type", "application/json");
  }

  if (response != NULL) {
    MHD_add_response_header(response, "Connection", "close");
    ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
  } else {
    char not_found[] = "resource not found";
    response = MHD_create_response_from_buffer(std::strlen(not_found),
                                               static_cast<void*>(not_found),
                                               MHD_RESPMEM_MUST_COPY);
    ret = MHD_queue_response(connection, MHD_HTTP_NOT_FOUND, response);
  }

  MHD_destroy_response(response);
  return ret;
}

Json::Value HttpServer::get_proc_status(const std::vector<std::string> &keys) {
  Json::Value root;
  Json::Value request_status;
  Json::Value threads_status;

  int success = 0;
  int fail = 0;

  std::string main_pid_str =
    std::to_string(static_cast<long long>(HttpServer::main_pid));

  if (statsdcc::read_status(
       "/proc/" +
       main_pid_str +
       "/status",
       keys,
       threads_status)) {
    root["threads"]["main"]= threads_status;
  } else {
    ++fail;
  }

  std::string path_prefix = "/proc/" + main_pid_str + "/task/";
  if (DIR * dir = opendir(path_prefix.c_str())) {
    closedir(dir);
  } else {
    path_prefix = "/proc/";
  }

  // servers stats
  int id = 0;
  for (auto server_ptr_itr = ::servers.cbegin();
       server_ptr_itr != ::servers.cend();
       ++server_ptr_itr) {
    std::vector<int> ttids = (*server_ptr_itr)->get_tids();
    for (unsigned int i = 0 ; i < ttids.size(); ++i) {
      if (statsdcc::read_status(
            path_prefix +
            std::to_string(static_cast<long long>(ttids[i])) +
            "/status",
            keys,
            threads_status)) {
        root["threads"]["servers"]
          [std::to_string(static_cast<long long>(++id))] = threads_status;
        ++success;
      } else {
        ++fail;
      }
    }
  }

  // workers stats
  id = 0;
  for (auto worker_ptr_itr = ::workers.cbegin();
       worker_ptr_itr != ::workers.cend();
       ++worker_ptr_itr) {
    if (statsdcc::read_status(
          path_prefix +
          std::to_string(
            static_cast<long long>((*worker_ptr_itr)->get_tids()[0])) +
          "/status",
          keys,
          threads_status)) {
      root["threads"]["workers"][std::to_string(static_cast<long long>(++id))] =
        threads_status;
      ++success;
    } else {
      ++fail;
    }
  }

  // clock stats
  if (statsdcc::read_status(
        path_prefix +
        std::to_string(static_cast<long long int>(::_clock->get_tids()[0])) +
        "/status",
        keys,
        threads_status)) {
    root["threads"]["clock"]["1"] =
      threads_status;
    ++success;
  } else {
    ++fail;
  }

  request_status["total"] = success + fail;
  request_status["success"] = success;
  request_status["failed"] = fail;

  root["status"] = request_status;

  return root;
}

Json::Value HttpServer::get_sys_info(const std::vector<std::string> &keys) {
  Json::Value root;
  Json::Value memstat;
  Json::Value cpustat;

  if (statsdcc::read_status("/proc/meminfo", keys, memstat)) {
    root["meminfo"] = memstat;
  }

  if (statsdcc::read_status("/proc/cpuinfo", keys, cpustat)) {
    root["cpuinfo"] = cpustat;
  }

  return root;
}

Json::Value HttpServer::get_app_status() {
  Json::Value root;

  // workers app stats
  long long id = 0;
  for (auto worker_ptr_itr = ::workers.cbegin();
       worker_ptr_itr != ::workers.cend();
       ++worker_ptr_itr) {
    root[std::to_string(++id)]["metrics_rate"] =
      static_cast<Json::Value::UInt64>((*worker_ptr_itr)->metrics_rate);
    root[std::to_string(id)]["metrics_dropped"] =
      static_cast<Json::Value::UInt64>((*worker_ptr_itr)->metrics_dropped);

    auto frequency = (*worker_ptr_itr)->get_metrics_frequency();
    std::vector<std::pair<std::string, long long int>> sorted_freq;

    for (auto frequency_itr = frequency.begin();
         frequency_itr != frequency.end();
         ++frequency_itr) {
      sorted_freq.push_back(*frequency_itr);
    }

    std::sort(sorted_freq.begin(), sorted_freq.end(),
              boost::bind(&std::pair<std::string, long long int>::second, _1) >
              boost::bind(&std::pair<std::string, long long int>::second, _2));

    int i = 0;
    for (auto itr = sorted_freq.cbegin();
         itr != sorted_freq.cend();
         ++itr) {
      root[std::to_string(id)]["hot_metrics"][itr->first] = Json::UInt64(itr->second);
      if (++i == 5) break;
    }

    root[std::to_string(id)]["bad_lines_seen"] =
      static_cast<Json::Value::UInt64>((*worker_ptr_itr)->bad_lines_seen);
  }

  return root;
}

}  // namespace aggregator
}  // namespace http
}  // namespace servers
}  // namespace net
}  // namespace statsdcc
