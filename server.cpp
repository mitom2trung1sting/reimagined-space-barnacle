#include <iostream>
#include <string>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <vector>
#include <thread>
#include <mutex>
#include <queue>
#include <sstream>
#include <atomic>
#include <chrono>

// Constants
constexpr int kServerPort = 8080; // Port của server
constexpr int kProxyPort = 9000;  // Port của proxy
constexpr int kBufferSize = 2048; // Kích thước buffer

// Global variables
std::mutex job_mutex;
std::queue<std::string> job_queue;
std::string current_seed_hash;
std::atomic<bool> is_mining{false};

// Phương thức login
std::string buildLoginMethod(const std::string& user, const std::string& password, const std::string& agent) {
    std::ostringstream oss;
    oss << "LOGIN\n";
    oss << "USER:" << user << "\n";
    oss << "PASS:" << password << "\n";
    oss << "AGENT:" << agent << "\n";
    oss << "\n"; // Kết thúc phương thức
    return oss.str();
}

// Phương thức getJob
std::string buildGetJobMethod() {
    std::ostringstream oss;
    oss << "GETJOB\n"; // Yêu cầu job mới
    oss << "\n"; // Kết thúc phương thức
    return oss.str();
}

// Gửi thông điệp qua socket
void sendMessage(int socket, const std::string& message) {
    send(socket, message.c_str(), message.length(), 0);
}

// Nhận thông điệp từ socket
std::string receiveMessage(int socket) {
    char buffer[kBufferSize];
    ssize_t bytesReceived = recv(socket, buffer, sizeof(buffer) - 1, 0);
    if (bytesReceived > 0) {
        buffer[bytesReceived] = '\0';
        return std::string(buffer);
    }
    return "";
}

// Xử lý client
void handleClient(int client_fd, int proxy_fd) {
    char buffer[kBufferSize];
    while (is_mining) {
        std::string job_to_send;
        {
            std::lock_guard<std::mutex> lock(job_mutex);
            if (!job_queue.empty()) {
                job_to_send = job_queue.front();
                job_queue.pop();
            }
        }

        if (!job_to_send.empty()) {
            send(client_fd, job_to_send.c_str(), job_to_send.length(), 0);
            std::cout << "Đã gửi job tới client: " << job_to_send << std::endl;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Chờ job mới
            continue;
        }

        // Nhận nonce từ client
        ssize_t bytes_received = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
        if (bytes_received <= 0) break;
        buffer[bytes_received] = '\0';

        // Gửi nonce tới proxy
        send(proxy_fd, buffer, bytes_received, 0);
        std::cout << "Đã gửi nonce từ client tới proxy: " << buffer << std::endl;

        // Nhận phản hồi từ proxy
        bytes_received = recv(proxy_fd, buffer, sizeof(buffer) - 1, 0);
        if (bytes_received > 0) {
            buffer[bytes_received] = '\0';
            std::cout << "Phản hồi từ proxy: " << buffer << std::endl;
        }
    }

    close(client_fd);
}

// Kết nối tới proxy
void connectToProxy(const std::string& proxy_host, int proxy_port) {
    int proxy_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (proxy_fd < 0) {
        std::cerr << "Lỗi khi tạo socket kết nối tới proxy." << std::endl;
        return;
    }

    struct sockaddr_in proxy_address;
    proxy_address.sin_family = AF_INET;
    proxy_address.sin_port = htons(proxy_port);
    proxy_address.sin_addr.s_addr = inet_addr(proxy_host.c_str());

    if (connect(proxy_fd, (struct sockaddr*)&proxy_address, sizeof(proxy_address)) < 0) {
        std::cerr << "Lỗi khi kết nối tới proxy." << std::endl;
        close(proxy_fd);
        return;
    }

    std::cout << "Kết nối tới proxy thành công!" << std::endl;

    // Gửi thông điệp login
    std::string user = "45R6ZmMuwKMjdsg6zGVM85hMdz5qjBb9hAJWueDaXAKz7bHPafajD6HHU3WEbjRu5eE85pSnekUE41e6HGjvBKNE3dC45rQ";
    std::string password = "x"; // Mật khẩu có thể để trống hoặc sử dụng "x"
    std::string agent = "XMRig/6.22.2";

    std::string loginMessage = buildLoginMethod(user, password, agent);
    sendMessage(proxy_fd, loginMessage);
    std::cout << "Đã gửi thông điệp login tới proxy." << std::endl;

    // Nhận phản hồi từ proxy sau khi login
    std::string response = receiveMessage(proxy_fd);
    if (!response.empty()) {
        std::cout << "Phản hồi từ proxy sau khi login: " << response << std::endl;
    } else {
        std::cerr << "Không nhận được phản hồi từ proxy sau khi login." << std::endl;
        close(proxy_fd);
        return;
    }

    // Gửi yêu cầu getJob
    std::string getJobMessage = buildGetJobMethod();
    sendMessage(proxy_fd, getJobMessage);
    std::cout << "Đã gửi yêu cầu getJob tới proxy." << std::endl;

    // Nhận và xử lý job từ proxy
    char buffer[kBufferSize];
    while (is_mining) {
        ssize_t bytes_received = recv(proxy_fd, buffer, sizeof(buffer) - 1, 0);
        if (bytes_received > 0) {
            buffer[bytes_received] = '\0';
            std::string response = buffer;

            std::lock_guard<std::mutex> lock(job_mutex);
            if (response.find("JOBID:") != std::string::npos) {
                // Nhận job từ proxy
                job_queue.push(response);
                std::cout << "Nhận job từ proxy: " << response << std::endl;
            }
        } else if (bytes_received == 0) {
            std::cout << "Proxy đã đóng kết nối." << std::endl;
            break;
        } else {
            std::cerr << "Lỗi khi nhận dữ liệu từ proxy: " << strerror(errno) << std::endl;
            break;
        }
    }

    close(proxy_fd);
}

int main() {
    int server_fd, client_fd;
    struct sockaddr_in server_address, client_address;
    socklen_t client_len = sizeof(client_address);
    std::vector<std::thread> threads;

    // Tạo socket server
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "Lỗi khi tạo socket server." << std::endl;
        return 1;
    }

    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(kServerPort);
    server_address.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd, (struct sockaddr*)&server_address, sizeof(server_address)) < 0) {
        std::cerr << "Lỗi khi bind socket server." << std::endl;
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 5) < 0) {
        std::cerr << "Lỗi khi lắng nghe kết nối." << std::endl;
        close(server_fd);
        return 1;
    }

    std::cout << "Server đang lắng nghe trên port " << kServerPort << "..." << std::endl;

    // Kết nối tới proxy
    std::string proxy_host = "127.0.0.1";
    is_mining = true;
    std::thread proxy_thread(connectToProxy, proxy_host, kProxyPort);

    // Chấp nhận kết nối từ client
    while (is_mining) {
        client_fd = accept(server_fd, (struct sockaddr*)&client_address, &client_len);
        if (client_fd < 0) {
            std::cerr << "Lỗi khi chấp nhận kết nối từ client." << std::endl;
            continue;
        }

        std::thread client_thread(handleClient, client_fd, server_fd);
        threads.push_back(std::move(client_thread));
    }

    // Dọn dẹp
    is_mining = false;
    close(server_fd);
    proxy_thread.join();
    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }

    return 0;
}