#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using cf32 = std::complex<float>;

struct Config {
    std::string input_path;
    bool use_stdin = false;
    double sample_rate = 1010526.0;
    std::string sr = "150k";
    std::string gi = "1/8";
    std::vector<int> fft_candidates;
    int symbols_to_analyze = 120;
    std::string artifact_prefix = "probe";
    bool debug_input = false;
    bool debug_sync = false;
    bool debug_fft = false;
    bool debug_qpsk = false;
    bool report_to_stdout = false;
};

struct IQStats {
    double mean_i = 0.0;
    double mean_q = 0.0;
    double rms = 0.0;
    double peak = 0.0;
    uint64_t clipped = 0;
    size_t samples = 0;
};

struct CandidateResult {
    int fft_size = 0;
    int gi_samples = 0;
    int symbol_samples = 0;
    int best_offset = 0;
    double corr_ratio = 0.0;
    double corr_mean = 0.0;
    double corr_peak = 0.0;
    double cfo_hz = 0.0;
};

struct QpskMetrics {
    size_t used_symbols = 0;
    size_t used_bins = 0;
    size_t used_points = 0;
    double evm_rms = 0.0;
    double evm_db = 0.0;
    double mean_radius = 0.0;
    double radius_stddev = 0.0;
    double spread_i = 0.0;
    double spread_q = 0.0;
    double qpsk_score = 0.0;
};

static void die(const std::string& msg) {
    throw std::runtime_error(msg);
}

static std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

static int parse_gi_denominator(const std::string& gi) {
    if (gi == "1/4") return 4;
    if (gi == "1/8") return 8;
    if (gi == "1/16") return 16;
    if (gi == "1/32") return 32;
    die("Unsupported GI. Use one of: 1/4, 1/8, 1/16, 1/32");
    return 8;
}

static std::vector<int> default_fft_candidates(const std::string& sr) {
    if (sr == "125k") return {512, 1024, 2048, 4096};
    if (sr == "150k") return {512, 1024, 2048, 4096};
    if (sr == "250k") return {512, 1024, 2048, 4096};
    if (sr == "333k") return {512, 1024, 2048};
    if (sr == "500k") return {256, 512, 1024, 2048};
    return {512, 1024, 2048, 4096};
}

static Config parse_args(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto need_value = [&](const std::string& name) -> std::string {
            if (i + 1 >= argc) die("Missing value for " + name);
            return argv[++i];
        };

        if (arg == "--input") {
            cfg.input_path = need_value(arg);
        } else if (arg == "--stdin") {
            cfg.use_stdin = true;
        } else if (arg == "--sample-rate") {
            cfg.sample_rate = std::stod(need_value(arg));
        } else if (arg == "--sr") {
            cfg.sr = need_value(arg);
        } else if (arg == "--gi") {
            cfg.gi = need_value(arg);
        } else if (arg == "--fft-candidates") {
            cfg.fft_candidates.clear();
            for (const auto& part : split(need_value(arg), ',')) {
                cfg.fft_candidates.push_back(std::stoi(part));
            }
        } else if (arg == "--symbols") {
            cfg.symbols_to_analyze = std::stoi(need_value(arg));
        } else if (arg == "--artifact-prefix") {
            cfg.artifact_prefix = need_value(arg);
        } else if (arg == "--debug") {
            const auto flags = split(need_value(arg), ',');
            for (const auto& f : flags) {
                if (f == "input") cfg.debug_input = true;
                else if (f == "sync") cfg.debug_sync = true;
                else if (f == "fft") cfg.debug_fft = true;
                else if (f == "qpsk") cfg.debug_qpsk = true;
                else if (f == "all") cfg.debug_input = cfg.debug_sync = cfg.debug_fft = cfg.debug_qpsk = true;
            }
        } else if (arg == "--report-to-stdout") {
            cfg.report_to_stdout = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout
                << "Usage: rbdvbt_qpsk_probe --input capture.cs16 [options]\n"
                << "  or   cat capture.cs16 | rbdvbt_qpsk_probe --stdin [options]\n\n"
                << "Options:\n"
                << "  --sample-rate <hz>      default 1010526\n"
                << "  --sr <125k|150k|250k|333k|500k>\n"
                << "  --gi <1/4|1/8|1/16|1/32>\n"
                << "  --fft-candidates a,b,c  default depends on --sr\n"
                << "  --symbols <n>           default 120\n"
                << "  --artifact-prefix <p>   default probe\n"
                << "  --debug input,sync,fft,qpsk\n"
                << "  --report-to-stdout\n";
            std::exit(0);
        } else {
            die("Unknown argument: " + arg);
        }
    }

    if (cfg.input_path.empty() && !cfg.use_stdin) {
        die("Use --input <file> or --stdin");
    }
    if (cfg.input_path.empty() == false && cfg.use_stdin) {
        die("Use either --input or --stdin, not both");
    }
    if (cfg.fft_candidates.empty()) {
        cfg.fft_candidates = default_fft_candidates(cfg.sr);
    }
    return cfg;
}

static std::vector<int16_t> read_all_bytes_as_i16(std::istream& in) {
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() % 4 != 0) {
        std::cerr << "[warn] Input byte count is not a multiple of 4; truncating tail bytes\n";
        bytes.resize(bytes.size() - (bytes.size() % 4));
    }
    std::vector<int16_t> data(bytes.size() / 2);
    for (size_t i = 0; i < data.size(); ++i) {
        uint8_t lo = static_cast<uint8_t>(bytes[2 * i]);
        uint8_t hi = static_cast<uint8_t>(bytes[2 * i + 1]);
        data[i] = static_cast<int16_t>((static_cast<uint16_t>(hi) << 8) | lo);
    }
    return data;
}

static std::vector<cf32> load_iq(const Config& cfg, IQStats& stats) {
    std::vector<int16_t> raw;
    if (cfg.use_stdin) {
        raw = read_all_bytes_as_i16(std::cin);
    } else {
        std::ifstream fin(cfg.input_path, std::ios::binary);
        if (!fin) die("Could not open input file: " + cfg.input_path);
        raw = read_all_bytes_as_i16(fin);
    }
    if (raw.size() < 4) die("Input too small");
    if (raw.size() % 2 != 0) raw.pop_back();

    const size_t n = raw.size() / 2;
    std::vector<cf32> iq;
    iq.reserve(n);

    double sum_i = 0.0, sum_q = 0.0, sum_p = 0.0;
    double peak = 0.0;
    uint64_t clipped = 0;
    for (size_t k = 0; k < n; ++k) {
        const int16_t i = raw[2 * k + 0];
        const int16_t q = raw[2 * k + 1];
        if (i == std::numeric_limits<int16_t>::min() || i == std::numeric_limits<int16_t>::max() ||
            q == std::numeric_limits<int16_t>::min() || q == std::numeric_limits<int16_t>::max()) {
            ++clipped;
        }
        const float fi = static_cast<float>(i) / 32768.0f;
        const float fq = static_cast<float>(q) / 32768.0f;
        iq.emplace_back(fi, fq);
        sum_i += fi;
        sum_q += fq;
        const double p = static_cast<double>(fi) * fi + static_cast<double>(fq) * fq;
        sum_p += p;
        peak = std::max(peak, std::sqrt(p));
    }

    stats.mean_i = sum_i / static_cast<double>(n);
    stats.mean_q = sum_q / static_cast<double>(n);
    stats.rms = std::sqrt(sum_p / static_cast<double>(n));
    stats.peak = peak;
    stats.clipped = clipped;
    stats.samples = n;

    for (auto& s : iq) {
        s -= cf32(static_cast<float>(stats.mean_i), static_cast<float>(stats.mean_q));
    }

    return iq;
}

static void fft_inplace(std::vector<cf32>& a) {
    const size_t n = a.size();
    if (n == 0 || (n & (n - 1)) != 0) die("FFT size must be a power of two");

    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }

    for (size_t len = 2; len <= n; len <<= 1) {
        const float ang = -2.0f * static_cast<float>(M_PI) / static_cast<float>(len);
        const cf32 wlen(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            cf32 w(1.0f, 0.0f);
            for (size_t j = 0; j < len / 2; ++j) {
                const cf32 u = a[i + j];
                const cf32 v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

static std::vector<cf32> fftshifted(std::vector<cf32> bins) {
    const size_t n = bins.size();
    std::rotate(bins.begin(), bins.begin() + n / 2, bins.end());
    return bins;
}

static CandidateResult evaluate_candidate(
    const std::vector<cf32>& iq,
    double sample_rate,
    int fft_size,
    int gi_den,
    int symbols_to_scan,
    bool debug_sync)
{
    CandidateResult out;
    out.fft_size = fft_size;
    out.gi_samples = fft_size / gi_den;
    out.symbol_samples = fft_size + out.gi_samples;

    if (out.gi_samples <= 0) die("GI samples became zero; FFT candidate too small for GI");
    if (iq.size() < static_cast<size_t>(out.symbol_samples * (symbols_to_scan + 2))) {
        return out;
    }

    const int search_span = out.symbol_samples;
    std::vector<double> scores(search_span, 0.0);
    std::vector<double> cfo(search_span, 0.0);

    for (int ofs = 0; ofs < search_span; ++ofs) {
        double metric_acc = 0.0;
        double power_acc = 0.0;
        cf32 corr_acc(0.0f, 0.0f);
        int used = 0;

        for (int sym = 0; sym < symbols_to_scan; ++sym) {
            const int base = ofs + sym * out.symbol_samples;
            if (base + out.symbol_samples >= static_cast<int>(iq.size())) break;
            cf32 corr(0.0f, 0.0f);
            double p1 = 0.0, p2 = 0.0;
            for (int k = 0; k < out.gi_samples; ++k) {
                const auto a = iq[base + k];
                const auto b = iq[base + fft_size + k];
                corr += std::conj(a) * b;
                p1 += std::norm(a);
                p2 += std::norm(b);
            }
            const double denom = std::sqrt(std::max(1e-12, p1 * p2));
            const double normcorr = std::abs(corr) / denom;
            metric_acc += normcorr;
            power_acc += 1.0;
            corr_acc += corr;
            ++used;
        }

        if (used > 0) {
            scores[ofs] = metric_acc / static_cast<double>(used);
            const double phase = std::arg(corr_acc);
            const double delay_s = static_cast<double>(fft_size) / sample_rate;
            cfo[ofs] = phase / (2.0 * M_PI * delay_s);
        }
    }

    auto max_it = std::max_element(scores.begin(), scores.end());
    const int best = static_cast<int>(std::distance(scores.begin(), max_it));
    const double peak = *max_it;
    std::vector<double> tmp = scores;
    std::nth_element(tmp.begin(), tmp.begin() + tmp.size() / 2, tmp.end());
    const double med = tmp[tmp.size() / 2];

    out.best_offset = best;
    out.corr_peak = peak;
    out.corr_mean = med;
    out.corr_ratio = peak / std::max(1e-9, med);
    out.cfo_hz = cfo[best];

    if (debug_sync) {
        std::cerr << "[sync] candidate fft=" << fft_size
                  << " gi=" << out.gi_samples
                  << " sym=" << out.symbol_samples
                  << " best_ofs=" << best
                  << " peak=" << peak
                  << " median=" << med
                  << " ratio=" << out.corr_ratio
                  << " cfo=" << out.cfo_hz << " Hz\n";
    }

    return out;
}

static CandidateResult choose_best_candidate(const std::vector<cf32>& iq, const Config& cfg) {
    const int gi_den = parse_gi_denominator(cfg.gi);
    CandidateResult best;
    best.corr_ratio = -1.0;

    for (int fft_size : cfg.fft_candidates) {
        auto r = evaluate_candidate(iq, cfg.sample_rate, fft_size, gi_den, std::max(20, cfg.symbols_to_analyze), cfg.debug_sync);
        if (r.symbol_samples == 0) continue;
        if (r.corr_ratio > best.corr_ratio) best = r;
    }
    if (best.corr_ratio < 0.0) die("Could not evaluate any FFT candidate; capture too short?");
    return best;
}

static std::vector<std::vector<cf32>> extract_bins(const std::vector<cf32>& iq, const CandidateResult& cand, int symbols_to_analyze) {
    std::vector<std::vector<cf32>> all;
    all.reserve(symbols_to_analyze);

    const int start = cand.best_offset;
    for (int s = 0; s < symbols_to_analyze; ++s) {
        const int base = start + s * cand.symbol_samples + cand.gi_samples;
        if (base + cand.fft_size > static_cast<int>(iq.size())) break;
        std::vector<cf32> sym(cand.fft_size);
        std::copy(iq.begin() + base, iq.begin() + base + cand.fft_size, sym.begin());
        fft_inplace(sym);
        all.push_back(fftshifted(std::move(sym)));
    }
    return all;
}

static std::vector<double> mean_bin_magnitude(const std::vector<std::vector<cf32>>& all_bins) {
    if (all_bins.empty()) return {};
    const int n = static_cast<int>(all_bins.front().size());
    std::vector<double> mag(n, 0.0);
    for (const auto& sym : all_bins) {
        for (int k = 0; k < n; ++k) mag[k] += std::abs(sym[k]);
    }
    for (double& v : mag) v /= static_cast<double>(all_bins.size());
    return mag;
}

static std::vector<int> select_active_bins(const std::vector<std::vector<cf32>>& all_bins) {
    const auto mag = mean_bin_magnitude(all_bins);
    if (mag.empty()) return {};

    std::vector<double> sorted = mag;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    const double median = sorted[sorted.size() / 2];
    const double threshold = median * 3.0;

    std::vector<int> bins;
    const int n = static_cast<int>(mag.size());
    const int dc = n / 2;
    for (int k = 0; k < n; ++k) {
        if (std::abs(k - dc) < 3) continue;
        if (mag[k] > threshold) bins.push_back(k);
    }
    return bins;
}

static cf32 estimate_qpsk_channel_from_fourth_power(const std::vector<cf32>& samples) {
    cf32 acc(0.0f, 0.0f);
    double mean_mag = 0.0;
    for (const auto& x : samples) {
        acc += x * x * x * x;
        mean_mag += std::abs(x);
    }
    mean_mag /= std::max<size_t>(1, samples.size());
    const float phase = std::arg(acc) / 4.0f;
    const float amp = static_cast<float>(std::max(1e-6, mean_mag));
    return std::polar(amp, phase);
}

static double wrap_pi(double x) {
    while (x > M_PI) x -= 2.0 * M_PI;
    while (x < -M_PI) x += 2.0 * M_PI;
    return x;
}

static void write_constellation_svg(const std::string& path, const std::vector<cf32>& pts) {
    std::ofstream f(path);
    const int w = 900, h = 900;
    f << "<svg xmlns='http://www.w3.org/2000/svg' width='" << w << "' height='" << h << "' viewBox='0 0 " << w << " " << h << "'>\n";
    f << "<rect width='100%' height='100%' fill='white'/>\n";
    f << "<line x1='0' y1='" << h/2 << "' x2='" << w << "' y2='" << h/2 << "' stroke='#bbb'/>\n";
    f << "<line x1='" << w/2 << "' y1='0' x2='" << w/2 << "' y2='" << h << "' stroke='#bbb'/>\n";
    const double scale = 250.0;
    for (const auto& p : pts) {
        double x = w / 2.0 + static_cast<double>(p.real()) * scale;
        double y = h / 2.0 - static_cast<double>(p.imag()) * scale;
        if (x < 0 || x >= w || y < 0 || y >= h) continue;
        f << "<circle cx='" << x << "' cy='" << y << "' r='1.2' fill='rgba(0,0,160,0.22)'/>\n";
    }
    const std::vector<cf32> ideal = {
        {+0.7071f, +0.7071f}, {-0.7071f, +0.7071f},
        {-0.7071f, -0.7071f}, {+0.7071f, -0.7071f}
    };
    for (const auto& p : ideal) {
        double x = w / 2.0 + static_cast<double>(p.real()) * scale;
        double y = h / 2.0 - static_cast<double>(p.imag()) * scale;
        f << "<circle cx='" << x << "' cy='" << y << "' r='6' fill='red'/>\n";
    }
    f << "</svg>\n";
}

static void write_spectrum_svg(const std::string& path, const std::vector<double>& mag) {
    std::ofstream f(path);
    const int w = 1200, h = 500;
    f << "<svg xmlns='http://www.w3.org/2000/svg' width='" << w << "' height='" << h << "' viewBox='0 0 " << w << " " << h << "'>\n";
    f << "<rect width='100%' height='100%' fill='white'/>\n";
    if (!mag.empty()) {
        const double maxv = *std::max_element(mag.begin(), mag.end());
        const double minv = *std::min_element(mag.begin(), mag.end());
        f << "<polyline fill='none' stroke='navy' stroke-width='1' points='";
        for (size_t i = 0; i < mag.size(); ++i) {
            double x = (static_cast<double>(i) / std::max<size_t>(1, mag.size() - 1)) * (w - 20) + 10;
            double y = h - 10 - (mag[i] - minv) / std::max(1e-9, maxv - minv) * (h - 20);
            f << x << "," << y << " ";
        }
        f << "'/>\n";
    }
    f << "</svg>\n";
}

static QpskMetrics analyze_qpsk(
    const std::vector<std::vector<cf32>>& all_bins,
    const std::vector<int>& active_bins,
    const std::string& prefix,
    bool debug_fft,
    bool debug_qpsk)
{
    QpskMetrics m;
    m.used_symbols = all_bins.size();
    m.used_bins = active_bins.size();

    if (all_bins.empty() || active_bins.empty()) return m;

    const int n_symbols = static_cast<int>(all_bins.size());
    std::vector<cf32> all_points;
    all_points.reserve(active_bins.size() * all_bins.size());

    for (int bin : active_bins) {
        std::vector<cf32> seq;
        seq.reserve(all_bins.size());
        for (const auto& sym : all_bins) seq.push_back(sym[bin]);
        const cf32 h = estimate_qpsk_channel_from_fourth_power(seq);
        for (const auto& x : seq) {
            all_points.push_back(x / h);
        }
    }

    std::vector<cf32> filtered;
    filtered.reserve(all_points.size());
    for (const auto& x : all_points) {
        const double r = std::abs(x);
        if (r > 0.15 && r < 4.0) filtered.push_back(x / static_cast<float>(r));
    }
    all_points = std::move(filtered);
    m.used_points = all_points.size();
    if (all_points.empty()) return m;

    double err2 = 0.0;
    double sum_r = 0.0, sum_r2 = 0.0;
    double sum_abs_i = 0.0, sum_abs_q = 0.0;
    std::vector<double> di, dq;
    di.reserve(all_points.size());
    dq.reserve(all_points.size());

    const float is2 = 1.0f / std::sqrt(2.0f);
    const std::vector<cf32> ideal = {{+is2,+is2},{-is2,+is2},{-is2,-is2},{+is2,-is2}};

    for (const auto& x : all_points) {
        double best_d2 = 1e9;
        cf32 best_pt;
        for (const auto& ref : ideal) {
            const double d2 = std::norm(x - ref);
            if (d2 < best_d2) {
                best_d2 = d2;
                best_pt = ref;
            }
        }
        err2 += best_d2;
        const double r = std::abs(x);
        sum_r += r;
        sum_r2 += r * r;
        di.push_back(std::abs(x.real()) - is2);
        dq.push_back(std::abs(x.imag()) - is2);
        sum_abs_i += std::abs(x.real());
        sum_abs_q += std::abs(x.imag());
        (void)best_pt;
    }

    auto stddev = [](const std::vector<double>& v) {
        if (v.empty()) return 0.0;
        double mean = std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
        double acc = 0.0;
        for (double x : v) acc += (x - mean) * (x - mean);
        return std::sqrt(acc / static_cast<double>(v.size()));
    };

    m.evm_rms = std::sqrt(err2 / static_cast<double>(all_points.size()));
    m.evm_db = 20.0 * std::log10(std::max(1e-9, m.evm_rms));
    m.mean_radius = sum_r / static_cast<double>(all_points.size());
    m.radius_stddev = std::sqrt(sum_r2 / static_cast<double>(all_points.size()) - m.mean_radius * m.mean_radius);
    m.spread_i = stddev(di);
    m.spread_q = stddev(dq);

    const double evm_term = std::clamp(1.0 - m.evm_rms / 0.7, 0.0, 1.0);
    const double radius_term = std::clamp(1.0 - m.radius_stddev / 0.5, 0.0, 1.0);
    const double axis_term = std::clamp(1.0 - (m.spread_i + m.spread_q) / 1.0, 0.0, 1.0);
    m.qpsk_score = 100.0 * (0.55 * evm_term + 0.20 * radius_term + 0.25 * axis_term);

    if (debug_qpsk) {
        std::cerr << "[qpsk] points=" << m.used_points
                  << " evm_rms=" << m.evm_rms
                  << " evm_db=" << m.evm_db
                  << " radius_mean=" << m.mean_radius
                  << " radius_std=" << m.radius_stddev
                  << " spread_i=" << m.spread_i
                  << " spread_q=" << m.spread_q
                  << " score=" << m.qpsk_score << "\n";
    }

    std::ofstream csv(prefix + "_constellation.csv");
    csv << "i,q\n";
    for (const auto& p : all_points) csv << p.real() << ',' << p.imag() << '\n';
    write_constellation_svg(prefix + "_constellation.svg", all_points);

    if (debug_fft) {
        std::ofstream bins(prefix + "_active_bins.csv");
        bins << "bin\n";
        for (int b : active_bins) bins << b << '\n';
    }

    return m;
}

static std::string verdict_from_score(double score) {
    if (score >= 75.0) return "strongly plausible QPSK";
    if (score >= 55.0) return "possibly valid QPSK";
    if (score >= 35.0) return "weak / marginal QPSK evidence";
    return "not convincing as QPSK";
}

int main(int argc, char** argv) {
    try {
        const Config cfg = parse_args(argc, argv);
        IQStats stats;
        const auto iq = load_iq(cfg, stats);

        if (cfg.debug_input) {
            std::cerr << std::fixed << std::setprecision(6)
                      << "[input] samples=" << stats.samples
                      << " mean_i=" << stats.mean_i
                      << " mean_q=" << stats.mean_q
                      << " rms=" << stats.rms
                      << " peak=" << stats.peak
                      << " clipped=" << stats.clipped << "\n";
        }

        const auto cand = choose_best_candidate(iq, cfg);
        const auto all_bins = extract_bins(iq, cand, cfg.symbols_to_analyze);
        const auto mag = mean_bin_magnitude(all_bins);
        const auto active_bins = select_active_bins(all_bins);
        const auto qpsk = analyze_qpsk(all_bins, active_bins, cfg.artifact_prefix, cfg.debug_fft, cfg.debug_qpsk);
        write_spectrum_svg(cfg.artifact_prefix + "_spectrum.svg", mag);
        std::ofstream magcsv(cfg.artifact_prefix + "_spectrum.csv");
        magcsv << "bin,mean_magnitude\n";
        for (size_t i = 0; i < mag.size(); ++i) magcsv << i << ',' << mag[i] << '\n';

        std::ostringstream report;
        report << std::fixed << std::setprecision(3)
               << "RB-DVB-T QPSK probe report\n"
               << "input_samples=" << stats.samples << "\n"
               << "sample_rate_hz=" << cfg.sample_rate << "\n"
               << "sr_preset=" << cfg.sr << "\n"
               << "gi=" << cfg.gi << "\n"
               << "best_fft_size=" << cand.fft_size << "\n"
               << "best_gi_samples=" << cand.gi_samples << "\n"
               << "best_symbol_samples=" << cand.symbol_samples << "\n"
               << "best_offset=" << cand.best_offset << "\n"
               << "gi_corr_peak=" << cand.corr_peak << "\n"
               << "gi_corr_median=" << cand.corr_mean << "\n"
               << "gi_corr_ratio=" << cand.corr_ratio << "\n"
               << "coarse_cfo_hz=" << cand.cfo_hz << "\n"
               << "symbols_used=" << qpsk.used_symbols << "\n"
               << "active_bins=" << qpsk.used_bins << "\n"
               << "points_used=" << qpsk.used_points << "\n"
               << "qpsk_evm_rms=" << qpsk.evm_rms << "\n"
               << "qpsk_evm_db=" << qpsk.evm_db << "\n"
               << "qpsk_spread_i=" << qpsk.spread_i << "\n"
               << "qpsk_spread_q=" << qpsk.spread_q << "\n"
               << "qpsk_score=" << qpsk.qpsk_score << "\n"
               << "verdict=" << verdict_from_score(qpsk.qpsk_score) << "\n"
               << "artifacts=" << cfg.artifact_prefix << "_constellation.svg,.csv ; "
               << cfg.artifact_prefix << "_spectrum.svg,.csv\n";

        if (cfg.report_to_stdout) {
            std::cout << report.str();
        } else {
            std::cerr << report.str();
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
