#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <string_view>
#include <sstream>
#include <chrono>
#include <thread>
#include <memory>
#include <numeric>
#include <algorithm>
#include <iomanip>
#include <csignal>
#include <cstring>
#include <cstdint>

// POSIX System Headers
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>

// Global flag for atomic signal handling
namespace {
    volatile std::sig_atomic_t g_running = 1;

    void signalHandler(int signum) {
        (void)signum;
        g_running = 0;
    }
}

// Struct to store raw CPU ticks read from /proc/stat
struct CpuTicks {
    uint64_t user{0};
    uint64_t nice{0};
    uint64_t system{0};
    uint64_t idle{0};
    uint64_t iowait{0};
    uint64_t irq{0};
    uint64_t softirq{0};
    uint64_t steal{0};

    uint64_t getIdleTicks() const noexcept {
        return idle + iowait;
    }

    uint64_t getTotalTicks() const noexcept {
        return user + nice + system + idle + iowait + irq + softirq + steal;
    }
};

// RAII Wrapper for File Descriptor or POSIX Resources if needed
class ProcStatReader {
public:
    ProcStatReader() {
        m_stream.open("/proc/stat", std::ios::in);
    }

    bool isOpen() const noexcept {
        return m_stream.is_open();
    }

    void rewind() {
        m_stream.clear();
        m_stream.seekg(0, std::ios::beg);
    }

    std::ifstream& getStream() {
        return m_stream;
    }

private:
    std::ifstream m_stream;
};

// Main CPU Monitor Manager
class CpuMonitor {
public:
    enum class State {
        Init,
        Run,
        Exit
    };

    CpuMonitor(int logIntervalSec = 0, std::string_view logFilePath = "")
        : m_logIntervalSec(logIntervalSec),
          m_logFilePath(logFilePath),
          m_currentState(State::Init) {}

    // Executes State Machine
    void execute() {
        while (m_currentState != State::Exit && g_running) {
            switch (m_currentState) {
                case State::Init:
                    if (!initPhase()) {
                        std::cerr << "[Error] Initialization failed.\n";
                        m_currentState = State::Exit;
                    } else {
                        m_currentState = State::Run; // Automatic transition to Run state
                    }
                    break;

                case State::Run:
                    runPhase();
                    m_currentState = State::Exit;
                    break;

                case State::Exit:
                    break;
            }
        }
    }

private:
    // Configurable settings
    int m_logIntervalSec{0};
    std::string m_logFilePath;
    State m_currentState{State::Init};

    // Pre-allocated memory structures (All allocated during Init phase)
    size_t m_coreCount{0};
    std::vector<CpuTicks> m_prevTicks;
    std::vector<CpuTicks> m_currTicks;
    std::vector<double> m_loadPercentages;
    std::vector<char> m_procBuffer;

    ProcStatReader m_statReader;
    std::ofstream m_logFileStream;
    std::chrono::steady_clock::time_point m_lastLogTime;

    // Helper: Detect number of CPU cores using POSIX API
    size_t detectCoreCount() const {
        long numCores = ::sysconf(_SC_NPROCESSORS_ONLN);
        return (numCores > 0) ? static_cast<size_t>(numCores) : 1;
    }

    // --- INIT STATE ---
    bool initPhase() {
        std::cout << "[Init State] Initializing system metrics & pre-allocating memory...\n";

        // 1. Setup Signal Handlers (POSIX compliant)
        struct sigaction sa;
        std::memset(&sa, 0, sizeof(sa));
        sa.sa_handler = signalHandler;
        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);

        // 2. Open /proc/stat
        if (!m_statReader.isOpen()) {
            std::cerr << "[Init Error] Failed to open /proc/stat\n";
            return false;
        }

        // 3. Core Detection & Reserve Memory (No dynamics allocations allowed during Run phase)
        m_coreCount = detectCoreCount();
        m_prevTicks.resize(m_coreCount);
        m_currTicks.resize(m_coreCount);
        m_loadPercentages.resize(m_coreCount, 0.0);

        // Pre-allocate text buffer for /proc/stat reading
        m_procBuffer.resize(8192);

        // 4. Open Log file if configured
        if (m_logIntervalSec > 0 && !m_logFilePath.empty()) {
            m_logFileStream.open(m_logFilePath, std::ios::out | std::ios::app);
            if (!m_logFileStream.is_open()) {
                std::cerr << "[Init Error] Cannot open log file: " << m_logFilePath << "\n";
                return false;
            }
            std::cout << "[Init State] Logging active every " << m_logIntervalSec 
                      << " seconds to file: " << m_logFilePath << "\n";
        }

        // 5. Initial snapshot read to prime deltas
        readCpuTicks(m_prevTicks);

        m_lastLogTime = std::chrono::steady_clock::now();

        std::cout << "[Init State] Detected " << m_coreCount << " CPU core(s).\n";
        std::cout << "[Init State] Success. Moving to Run state.\n\n";

        return true;
    }

    // Read current CPU ticks per core from /proc/stat without fresh allocations
    void readCpuTicks(std::vector<CpuTicks>& ticksVec) {
        m_statReader.rewind();
        auto& stream = m_statReader.getStream();

        std::string line;
        // Skip first aggregate 'cpu' line
        std::getline(stream, line);

        size_t coreIndex = 0;
        while (coreIndex < m_coreCount && std::getline(stream, line)) {
            if (line.compare(0, 3, "cpu") != 0) {
                break; // End of CPU metrics section
            }

            std::istringstream iss(line);
            std::string cpuHeader;
            CpuTicks ticks;

            iss >> cpuHeader
                >> ticks.user >> ticks.nice >> ticks.system >> ticks.idle
                >> ticks.iowait >> ticks.irq >> ticks.softirq >> ticks.steal;

            ticksVec[coreIndex] = ticks;
            coreIndex++;
        }
    }

    // Calculates non-allocating CPU percentages
    void computeLoad() {
        readCpuTicks(m_currTicks);

        for (size_t i = 0; i < m_coreCount; ++i) {
            uint64_t prevIdle = m_prevTicks[i].getIdleTicks();
            uint64_t currIdle = m_currTicks[i].getIdleTicks();

            uint64_t prevTotal = m_prevTicks[i].getTotalTicks();
            uint64_t currTotal = m_currTicks[i].getTotalTicks();

            uint64_t totalDelta = currTotal - prevTotal;
            uint64_t idleDelta = currIdle - prevIdle;

            if (totalDelta > 0) {
                double load = (1.0 - static_cast<double>(idleDelta) / static_cast<double>(totalDelta)) * 100.0;
                m_loadPercentages[i] = std::clamp(load, 0.0, 100.0);
            } else {
                m_loadPercentages[i] = 0.0;
            }

            m_prevTicks[i] = m_currTicks[i];
        }
    }

    // Print CPU load to standard stdout
    void printToStdout() {
        computeLoad();
        std::cout << "\n================ CPU Cores Load ================\n";
        for (size_t i = 0; i < m_coreCount; ++i) {
            std::cout << " Core #" << std::setw(2) << i << ": " 
                      << std::fixed << std::setprecision(2) << std::setw(6) 
                      << m_loadPercentages[i] << " %\n";
        }
        std::cout << "=================================================\n";
    }

    // Print CPU load to log file
    void printToFile() {
        if (!m_logFileStream.is_open()) return;

        computeLoad();

        auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        m_logFileStream << "Timestamp: " << ::ctime(&now);
        for (size_t i = 0; i < m_coreCount; ++i) {
            m_logFileStream << " Core #" << i << ": " 
                            << std::fixed << std::setprecision(2) 
                            << m_loadPercentages[i] << " %\n";
        }
        m_logFileStream << "-------------------------------------------------\n";
        m_logFileStream.flush();
    }

    // Check non-blocking input from std::cin using POSIX select()
    bool isInputAvailable() const {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);

        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000; // 100ms check timeout

        int result = ::select(STDIN_FILENO + 1, &readfds, nullptr, nullptr, &timeout);
        return (result > 0 && FD_ISSET(STDIN_FILENO, &readfds));
    }

    // --- RUN STATE ---
    void runPhase() {
        std::cout << "App entered RUN state.\n";
        std::cout << "Commands:\n";
        std::cout << "  [Press ENTER or type 'p']: Print CPU load to stdout\n";
        std::cout << "  [Type 'q' + ENTER]: Exit program\n\n";

        std::string cmdBuffer;
        cmdBuffer.reserve(32); // Pre-allocated string buffer

        while (g_running) {
            // Check timer for periodic logging to file
            if (m_logIntervalSec > 0) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_lastLogTime).count();

                if (elapsed >= m_logIntervalSec) {
                    printToFile();
                    m_lastLogTime = now;
                }
            }

            // Check non-blocking stdin input
            if (isInputAvailable()) {
                if (std::getline(std::cin, cmdBuffer)) {
                    if (cmdBuffer == "q" || cmdBuffer == "quit" || cmdBuffer == "exit") {
                        std::cout << "Exiting application...\n";
                        break;
                    } else {
                        printToStdout();
                    }
                }
            }
        }
    }
};

int main(int argc, char* argv[]) {
    int logIntervalSec = 0;
    std::string logFilePath;

    // Optional Command Line Arguments Parsing: <Interval_in_Sec> <Log_File_Path>
    if (argc >= 3) {
        logIntervalSec = std::atoi(argv[1]);
        logFilePath = argv[2];
    }

    CpuMonitor monitor(logIntervalSec, logFilePath);
    monitor.execute();

    return 0;
}