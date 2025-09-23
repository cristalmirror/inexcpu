
// cpu_monitor.cpp
// Compile with: 
//   Linux:   g++ -std=c++17 -O2 cpu_monitor.cpp -o cpu_monitor
//   Windows: cl /std:c++17 /O2 cpu_monitor.cpp PowrProf.lib
//            (o con MinGW: g++ -std=c++17 -O2 cpu_monitor.cpp -o cpu_monitor -lPowrProf)
// Nota: En Windows requiere PowrProf.lib para leer MHz por núcleo.

#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include <map>
#include <chrono>
#include <thread>

#ifdef _WIN32
  #define NOMINMAX
  #include <windows.h>
  #include <powrprof.h>   // CallNtPowerInformation, PROCESSOR_POWER_INFORMATION
  #pragma comment(lib, "PowrProf.lib")
  #include <tlhelp32.h>   // Toolhelp32Snapshot para enumerar procesos
#else
  #include <dirent.h>
  #include <cstring>
  #include <fstream>
  #include <regex>
  #include <unistd.h>
  #include <sys/types.h>
#endif

// ---------------------------- Utiles ----------------------------
static std::string human_mhz(double mhz) {
    std::ostringstream os;
    if (mhz >= 1000.0) {
        os << std::fixed << std::setprecision(2) << (mhz / 1000.0) << " GHz";
    } else {
        os << std::fixed << std::setprecision(0) << mhz << " MHz";
    }
    return os.str();
}

// ---------------------- Frecuencia por núcleo -------------------
#ifdef _WIN32
// Windows: usar CallNtPowerInformation(ProcessorInformation)
// Devuelve un array de PROCESSOR_POWER_INFORMATION, uno por lógica de CPU.
std::vector<double> get_core_frequencies_mhz() {
    std::vector<double> freqs;

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    ULONG nproc = si.dwNumberOfProcessors;
    if (nproc == 0) return freqs;

    const ULONG bufSize = nproc * sizeof(PROCESSOR_POWER_INFORMATION);
    std::vector<BYTE> buffer(bufSize);

    NTSTATUS st = CallNtPowerInformation(ProcessorInformation, nullptr, 0,
                                         buffer.data(), bufSize);
    if (st != 0) {
        // Fallback: Win32_Processor->CurrentClockSpeed no es por núcleo; omitimos.
        return freqs;
    }

    auto *ppi = reinterpret_cast<PROCESSOR_POWER_INFORMATION*>(buffer.data());
    freqs.resize(nproc);
    for (ULONG i = 0; i < nproc; ++i) {
        freqs[i] = static_cast<double>(ppi[i].CurrentMhz); // MHz actuales por CPU lógica
    }
    return freqs;
}
#else
// Linux: intentar sysfs cpufreq; si no está, usar /proc/cpuinfo (cpu MHz)
std::vector<double> get_core_frequencies_mhz() {
    std::vector<double> freqs;

    // Intento 1: /sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq (kHz)
    {
        // Contar CPUs lógicas por /sys/devices/system/cpu/cpu[0-9]+
        std::vector<int> cpus;
        DIR *d = opendir("/sys/devices/system/cpu");
        if (d) {
            std::regex re("^cpu([0-9]+)$");
            if (struct dirent *de; true) {
                while ((de = readdir(d)) != nullptr) {
                    std::cmatch m;
                    if (std::regex_match(de->d_name, m, re)) {
                        int id = std::stoi(m[1]);
                        cpus.push_back(id);
                    }
                }
            }
            closedir(d);
        }

        if (!cpus.empty()) {
            std::sort(cpus.begin(), cpus.end());
            freqs.resize(cpus.back() + 1, -1.0);

            for (int id : cpus) {
                std::ostringstream p;
                p << "/sys/devices/system/cpu/cpu" << id << "/cpufreq/scaling_cur_freq";
                std::ifstream f(p.str());
                if (f) {
                    long khz = 0;
                    f >> khz;
                    if (khz > 0) freqs[id] = khz / 1000.0; // a MHz
                }
            }

            // Si al menos una se leyó, devolver (aunque haya -1 en algunas)
            bool any = false;
            for (double x : freqs) if (x > 0) { any = true; break; }
            if (any) return freqs;
            freqs.clear(); // caer al fallback
        }
    }

    // Fallback: /proc/cpuinfo -> "processor" y "cpu MHz"
    {
        std::ifstream f("/proc/cpuinfo");
        if (!f) return freqs;

        std::string line;
        int currentCPU = -1;
        std::map<int, double> mhz;
        while (std::getline(f, line)) {
            if (line.rfind("processor", 0) == 0) {
                size_t pos = line.find(':');
                if (pos != std::string::npos) {
                    currentCPU = std::stoi(line.substr(pos + 1));
                }
            } else if (line.rfind("cpu MHz", 0) == 0 && currentCPU >= 0) {
                size_t pos = line.find(':');
                if (pos != std::string::npos) {
                    double v = std::stod(line.substr(pos + 1));
                    mhz[currentCPU] = v;
                }
            }
        }
        if (!mhz.empty()) {
            int maxId = mhz.rbegin()->first;
            freqs.assign(maxId + 1, -1.0);
            for (auto &kv : mhz) freqs[kv.first] = kv.second;
        }
        return freqs;
    }
}
#endif

// ----------------------- Lista de procesos ----------------------
#ifdef _WIN32
struct ProcInfo { DWORD pid; std::string name; };

std::vector<ProcInfo> list_processes() {
    std::vector<ProcInfo> out;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) {
        do {
            out.push_back(ProcInfo{ pe.th32ProcessID, pe.szExeFile });
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}
#else
struct ProcInfo { pid_t pid; std::string name; };

std::vector<ProcInfo> list_processes() {
    std::vector<ProcInfo> out;
    DIR *d = opendir("/proc");
    if (!d) return out;

    struct dirent *de;
    while ((de = readdir(d)) != nullptr) {
        // directorios numéricos = PIDs
        if (!std::all_of(de->d_name, de->d_name + std::strlen(de->d_name), ::isdigit))
            continue;

        pid_t pid = static_cast<pid_t>(std::stoi(de->d_name));
        std::string commPath = std::string("/proc/") + de->d_name + "/comm";
        std::ifstream f(commPath);
        std::string name;
        if (f && std::getline(f, name)) {
            if (!name.empty() && name.back() == '\n') name.pop_back();
        } else {
            // fallback: /status Name:
            std::ifstream s(std::string("/proc/") + de->d_name + "/status");
            std::string line;
            while (std::getline(s, line)) {
                if (line.rfind("Name:", 0) == 0) {
                    name = line.substr(5);
                    // trim
                    name.erase(0, name.find_first_not_of(" \t"));
                    break;
                }
            }
        }
        if (!name.empty()) {
            out.push_back(ProcInfo{ pid, name });
        }
    }
    closedir(d);
    std::sort(out.begin(), out.end(), [](const ProcInfo& a, const ProcInfo& b){
        return a.pid < b.pid;
    });
    return out;
}
#endif

// ----------------------------- Main -----------------------------
int main() {

  int count = 0;

  do {
  // Frecuencia por núcleo
    auto freqs = get_core_frequencies_mhz();

    std::cout << "=== Frecuencia actual por núcleo ===\n";
    count++;
    if (freqs.empty()) {
        std::cout << "No se pudo leer la frecuencia por núcleo en este sistema.\n";
    } else {
        for (size_t i = 0; i < freqs.size(); ++i) {
            if (freqs[i] > 0) {
                std::cout << "CPU " << i << ": " << human_mhz(freqs[i]) << "\n";
            } else {
                std::cout << "CPU " << i << ": N/D\n";
            }
	    count++;
        }
    }
    std::cout << "\n";
    count++;

    // Procesos
    std::cout << "=== Procesos en ejecución (PID, Nombre) ===\n";
    count++;
#ifdef _WIN32
    auto procs = list_processes();
    for (const auto& p : procs) {
        std::cout << p.pid << "  " << p.name << "\n";
	count++;
    }
#else
    auto procs = list_processes();
    for (const auto& p : procs) {
        std::cout << p.pid << "  " << p.name << "\n";
	count++;
    }
#endif

    std::cout << "x1b[" << count <<"A";
    std::cout << "x1b[2K\r";
    std::cout.flush();
    std::this_thread::sleep_for(std::chrono::seconds(1));
  } while(1);
    return 0;
}
