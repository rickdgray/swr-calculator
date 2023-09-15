#include <string>
#include <iostream>
#include <tuple>
#include <chrono>
#include <sstream>
#include <iomanip>

#include "data.hpp"
#include "portfolio.hpp"
#include "simulation.hpp"

#include "cpp_utils/parallel.hpp"
#include "cpp_utils/thread_pool.hpp"

#include <httplib.h>

namespace {

std::vector<std::string> parse_args(int argc, const char* argv[]){
    std::vector<std::string> args;

    for(int i = 0; i < argc - 1; ++i){
        args.emplace_back(argv[i+1]);
    }

    return args;
}

void multiple_wr(swr::scenario scenario){
    std::cout << "           Portfolio: \n";
    for (auto & position : scenario.portfolio) {
        std::cout << "             " << position.asset << ": " << position.allocation << "%\n";
    }

    std::cout << "\n";

    cpp::default_thread_pool pool(std::thread::hardware_concurrency());

    std::vector<swr::results> all_yearly_results;
    std::vector<swr::results> all_monthly_results;

    for (float wr = 3.0; wr < 5.1f; wr += 0.25f) {
        all_yearly_results.emplace_back();
        all_monthly_results.emplace_back();
    }

    size_t i = 0;

    for (float wr = 3.0; wr < 5.1f; wr += 0.25f) {
        pool.do_task(
                [&scenario, &all_yearly_results, &all_monthly_results](float wr, size_t i) {
                    auto my_scenario = scenario;

                    my_scenario.wr                 = wr;
                    my_scenario.withdraw_frequency = 12;

                    all_yearly_results[i] = swr::simulation(my_scenario);

                    my_scenario.withdraw_frequency = 1;
                    all_monthly_results[i]         = swr::simulation(my_scenario);
                },
                wr,
                i++);
    }

    pool.wait();

    i = 0;

    for (float wr = 3.0; wr < 5.1f; wr += 0.25f) {
        auto & yearly_results = all_yearly_results[i];
        auto & monthly_results = all_monthly_results[i];

        std::cout << wr << "% Success Rate (Yearly): (" << yearly_results.successes << "/" << (yearly_results.failures + yearly_results.successes) << ") " << yearly_results.success_rate << "%"
                  << " [" << yearly_results.tv_average << ":" << yearly_results.tv_median << ":" << yearly_results.tv_minimum << ":" << yearly_results.tv_maximum << "]" << std::endl;

        if (yearly_results.error) {
            std::cout << "Error in simulation: " << yearly_results.message << std::endl;
            return;
        }

        std::cout << wr << "% Success Rate (Monthly): (" << monthly_results.successes << "/" << (monthly_results.failures + monthly_results.successes) << ") " << monthly_results.success_rate << "%"
                  << " [" << monthly_results.tv_average << ":" << monthly_results.tv_median << ":" << monthly_results.tv_minimum << ":" << monthly_results.tv_maximum << "]" << std::endl;

        if (monthly_results.error) {
            std::cout << "Error in simulation: " << monthly_results.message << std::endl;
            return;
        }

        ++i;
    }
}

struct Graph {
    Graph(bool enabled) : enabled_(enabled) {
        if (enabled_) {
            std::cout << "[line-graph title=\"TODO\" ytitle=\"Success Rate <%>\" xtitle=\"Withdrawal Rate <%>\"";
        }
    }

    ~Graph() {
        if (enabled_) {
            std::stringstream legends;
            std::string       sep;
            for (auto& legend : legends_) {
                legends << sep << legend;
                sep = ",";
            }
            std::cout << " legends=\"" << legends.str() << "\"]";

            // TODO COmpute data

            std::cout << "[/line-graph]";
        }
    }

    void add_legend(std::string_view title) {
        legends_.emplace_back(title);
    }

    void set_data(const std::map<float, float> & data) {
        data_ = data;
    }

    std::vector<std::string> legends_;
    std::map<float, float> data_;
    const bool enabled_;
};

template <typename F>
void multiple_wr_graph(Graph & graph, const std::string & title, swr::scenario scenario, float start_wr, float end_wr, float add_wr, F functor){
    if (title.empty()) {
        std::stringstream ss;
        std::string sep;
        for (auto& position : scenario.portfolio) {
            if (position.allocation > 0) {
                if (position.asset == "ch_stocks") {
                    ss << sep << position.allocation << "% CH Stocks";
                } else if (position.asset == "ch_bonds") {
                    ss << sep << position.allocation << "% CH Bonds";
                } else {
                    ss << sep << position.allocation << "% " << position.asset;
                }
                sep = " ";
            }
        }
        graph.add_legend(ss.str());
    } else {
        graph.add_legend(title);
    }

    cpp::default_thread_pool pool(2 * std::thread::hardware_concurrency());
    std::map<float, float> results;

    for (float wr = start_wr; wr < end_wr + add_wr / 2.0f; wr += add_wr) {
        results[wr] = 0.0f;
    }

    std::atomic<bool> error = false;

    for (float wr = start_wr; wr < end_wr + add_wr / 2.0f; wr += add_wr) {
        pool.do_task([&results, &scenario, &error, &functor](float wr) {
            auto my_scenario = scenario;
            my_scenario.wr  = wr;
            auto res = swr::simulation(my_scenario);

            if (res.error) {
                error = false;
                std::cout << std::endl << "ERROR: " << res.message << std::endl;
            } else {
                results[wr] = functor(res);
            }
        }, wr);
    }

    pool.wait();

    if (!error) {
        graph.set_data(results);
    }
}

template <typename F>
void multiple_wr_sheets(const std::string & title, swr::scenario scenario, float start_wr, float end_wr, float add_wr, F functor){
    if (title.empty()) {
        for (auto& position : scenario.portfolio) {
            if (position.allocation > 0) {
                std::cout << position.allocation << "% " << position.asset << " ";
            }
        }
    } else {
        std::cout << title << " ";
    }

    cpp::default_thread_pool pool(2 * std::thread::hardware_concurrency());
    std::vector<float> results;

    for (float wr = start_wr; wr < end_wr + add_wr / 2.0f; wr += add_wr) {
        results.push_back(0.0f);
    }

    std::size_t i =0;
    std::atomic<bool> error = false;

    for (float wr = start_wr; wr < end_wr + add_wr / 2.0f; wr += add_wr) {
        pool.do_task([&results, &scenario, &error, &functor](float wr, size_t i) {
            auto my_scenario = scenario;
            my_scenario.wr  = wr;
            auto res = swr::simulation(my_scenario);

            if (res.error) {
                error = false;
                std::cout << std::endl << "ERROR: " << res.message << std::endl;
            } else {
                results[i] = functor(res);
            }
        }, wr, i++);
    }

    pool.wait();

    if (!error) {
        for (auto & res : results) {
            std::cout << ';' << res;
        }
    }

    std::cout << "\n";
}

void multiple_wr_success_sheets(const std::string & title, swr::scenario scenario, float start_wr, float end_wr, float add_wr){
    multiple_wr_sheets(title, scenario, start_wr, end_wr, add_wr, [](auto & results) {
        return results.success_rate;
    });
}

void multiple_wr_success_graph(Graph & graph, const std::string & title, swr::scenario scenario, float start_wr, float end_wr, float add_wr){
    multiple_wr_graph(graph, title, scenario, start_wr, end_wr, add_wr, [](auto & results) {
        return results.success_rate;
    });
}

void multiple_wr_withdrawn_sheets(const std::string & title, swr::scenario scenario, float start_wr, float end_wr, float add_wr){
    multiple_wr_sheets(title, scenario, start_wr, end_wr, add_wr, [](auto & results) {
        return results.withdrawn_per_year;
    });
}

void multiple_wr_duration_sheets(const std::string & title, swr::scenario scenario, float start_wr, float end_wr, float add_wr){
    multiple_wr_sheets(title, scenario, start_wr, end_wr, add_wr, [](auto & results) {
        return results.worst_duration;
    });
}

template <typename T>
void csv_print(const std::string& header, const std::vector<T> & values) {
    std::cout << header;
    for (auto & v : values) {
        std::cout << ";" << v;
    }
    std::cout << "\n";
}

void multiple_wr_tv_sheets(swr::scenario scenario, float start_wr, float end_wr, float add_wr){
    std::vector<float> min_tv;
    std::vector<float> max_tv;
    std::vector<float> avg_tv;
    std::vector<float> med_tv;

    for (float wr = start_wr; wr < end_wr + add_wr / 2.0f; wr += add_wr) {
        scenario.wr = wr;
        auto monthly_results = swr::simulation(scenario);
        min_tv.push_back(monthly_results.tv_minimum);
        max_tv.push_back(monthly_results.tv_maximum);
        avg_tv.push_back(monthly_results.tv_average);
        med_tv.push_back(monthly_results.tv_median);
    }

    csv_print("MIN", min_tv);
    csv_print("AVG", avg_tv);
    csv_print("MED", med_tv);
    csv_print("MAX", max_tv);
}

void failsafe_swr(swr::scenario & scenario, float start_wr, float end_wr, float step, float goal, std::ostream & out) {
    for (float wr = start_wr; wr >= end_wr; wr -= step) {
        scenario.wr = wr;
        auto monthly_results = swr::simulation(scenario);

        if (monthly_results.success_rate >= 100.0f - goal) {
            out<< ";" << wr;
            return;
        }
    }

    out << ";0";
}

void failsafe_swr(const std::string & title, swr::scenario & scenario, float start_wr, float end_wr, float step, std::ostream & out) {
    if (title.empty()) {
        for (auto& position : scenario.portfolio) {
            if (position.allocation > 0) {
                out << position.allocation << "% " << position.asset << " ";
            }
        }
    } else {
        out << title << " ";
    }

    failsafe_swr(scenario, start_wr, end_wr, step, 0.0f, out);
    failsafe_swr(scenario, start_wr, end_wr, step, 1.0f, out);
    failsafe_swr(scenario, start_wr, end_wr, step, 5.0f, out);
    failsafe_swr(scenario, start_wr, end_wr, step, 10.0f, out);
    failsafe_swr(scenario, start_wr, end_wr, step, 25.0f, out);
    out << '\n';
}

void multiple_rebalance_sheets(swr::scenario scenario, float start_wr, float end_wr, float add_wr){
    if (scenario.rebalance == swr::Rebalancing::THRESHOLD) {
        std::cout << scenario.threshold << " ";
    } else {
        std::cout << scenario.rebalance << " ";
    }

    for (float wr = start_wr; wr < end_wr + add_wr / 2.0f; wr += add_wr) {
        scenario.wr = wr;
        auto monthly_results = swr::simulation(scenario);
        std::cout << ';' << monthly_results.success_rate;
    }

    std::cout << "\n";
}

httplib::Server * server_ptr = nullptr;

void server_signal_handler(int signum) {
    std::cout << "Received signal (" << signum << ")" << std::endl;

    if (server_ptr) {
        server_ptr->stop();
    }
}

void install_signal_handler() {
    struct sigaction action;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    action.sa_handler = server_signal_handler;
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);

    std::cout << "Installed the signal handler" << std::endl;
}

bool check_parameters(const httplib::Request& req, httplib::Response& res, std::vector<const char*> parameters) {
    for (auto& param : parameters) {
        if (!req.has_param(param)) {
            res.set_content("Error: Missing parameter " + std::string(param), "text/plain");
            return false;
        }
    }

    return true;
}

void server_simple_api(const httplib::Request& req, httplib::Response& res) {
    if (!check_parameters(req, res, {"portfolio", "inflation", "years", "wr", "start", "end"})) {
        return;
    }

    auto start = std::chrono::high_resolution_clock::now();

    swr::scenario scenario;

    // Don't run for too long
    scenario.timeout_msecs = 200;

    // Parse the parameters
    auto portfolio_base = req.get_param_value("portfolio");
    scenario.portfolio  = swr::parse_portfolio(portfolio_base);
    auto inflation      = req.get_param_value("inflation");
    scenario.wr         = atof(req.get_param_value("wr").c_str());
    scenario.years      = atoi(req.get_param_value("years").c_str());
    scenario.start_year = atoi(req.get_param_value("start").c_str());
    scenario.end_year   = atoi(req.get_param_value("end").c_str());

    // Parse the optional parameters

    if (req.has_param("rebalance")) {
        auto param = req.get_param_value("rebalance");

        if (param == "none") {
            scenario.rebalance = swr::Rebalancing::NONE;
        } else if (param == "monthly") {
            scenario.rebalance = swr::Rebalancing::MONTHLY;
        } else if (param == "yearly") {
            scenario.rebalance = swr::Rebalancing::YEARLY;
        } else {
            scenario.rebalance = swr::Rebalancing::NONE;
        }
    } else {
        scenario.rebalance = swr::Rebalancing::NONE;
    }

    std::cout
        << "DEBUG: Request port=" << portfolio_base
        << " inf=" << inflation
        << " wr=" << scenario.wr
        << " years=" << scenario.years
        << " rebalance=" << scenario.rebalance
        << " start_year=" << scenario.start_year
        << " end_year=" << scenario.end_year
        << std::endl;

    // For now cannot be configured
    scenario.withdraw_frequency = 12;
    scenario.threshold = 0.0f;

    swr::normalize_portfolio(scenario.portfolio);

    scenario.values         = swr::load_values(scenario.portfolio);
    scenario.inflation_data = swr::load_inflation(scenario.values, inflation);

    if (scenario.values.empty()) {
        res.set_content(std::string("Error: Invalid portfolio: ") + portfolio_base, "text/plain");
        return;
    }

    if (scenario.inflation_data.empty()) {
        res.set_content("Error: Invalid inflation", "text/plain");
        return;
    }

    auto results = simulation(scenario);

    std::stringstream ss;

    ss << "{ \"results\": {\n";
    ss << "  \"successes\": " << results.successes << ",\n";
    ss << "  \"failures\": " << results.failures << ",\n";
    ss << "  \"success_rate\": " << results.success_rate << ",\n";
    ss << "  \"tv_average\": " << results.tv_average << ",\n";
    ss << "  \"tv_minimum\": " << results.tv_minimum << ",\n";
    ss << "  \"tv_maximum\": " << results.tv_maximum << ",\n";
    ss << "  \"tv_median\": " << results.tv_median << ",\n";
    ss << "  \"worst_duration\": " << results.worst_duration << ",\n";
    ss << "  \"worst_starting_month\": " << results.worst_starting_month << ",\n";
    ss << "  \"worst_starting_year\": " << results.worst_starting_year << ",\n";
    ss << "  \"worst_tv\": " << results.worst_tv << ",\n";
    ss << "  \"worst_tv_month\": " << results.worst_tv_month << ",\n";
    ss << "  \"worst_tv_year\": " << results.worst_tv_year << ",\n";
    ss << "  \"best_tv\": " << results.best_tv << ",\n";
    ss << "  \"best_tv_month\": " << results.best_tv_month << ",\n";
    ss << "  \"best_tv_year\": " << results.best_tv_year << ",\n";
    ss << "  \"message\": \"" << results.message << "\",\n";
    ss << "  \"error\": " << (results.error ? "true" : "false") << "\n";
    ss << "}}";

    res.set_content(ss.str(), "text/json");

    auto stop = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(stop - start).count();
    std::cout << "DEBUG: Simulated in " << duration << "ms" << std::endl;
}

void server_retirement_api(const httplib::Request& req, httplib::Response& res) {
    if (!check_parameters(req, res, {"expenses", "income", "wr", "sr", "nw"})) {
        return;
    }

    auto start = std::chrono::high_resolution_clock::now();

    swr::scenario scenario;

    // Don't run for too long
    scenario.timeout_msecs = 200;

    // Parse the parameters
    scenario.wr       = atof(req.get_param_value("wr").c_str());
    float sr       = atof(req.get_param_value("sr").c_str());
    float income   = atoi(req.get_param_value("income").c_str());
    float expenses = atoi(req.get_param_value("expenses").c_str());
    float nw       = atoi(req.get_param_value("nw").c_str());

    // Parse the optional parameters

    if (req.has_param("rebalance")) {
        auto param = req.get_param_value("rebalance");

        if (param == "none") {
            scenario.rebalance = swr::Rebalancing::NONE;
        } else if (param == "monthly") {
            scenario.rebalance = swr::Rebalancing::MONTHLY;
        } else if (param == "yearly") {
            scenario.rebalance = swr::Rebalancing::YEARLY;
        } else {
            scenario.rebalance = swr::Rebalancing::NONE;
        }
    } else {
        scenario.rebalance = swr::Rebalancing::NONE;
    }

    float returns   = 7.0f;

    std::cout
        << "DEBUG: Retirement Request wr=" << scenario.wr
        << " sr=" << sr
        << " nw=" << nw
        << " income=" << income
        << " expenses=" << expenses
        << " rebalance=" << scenario.rebalance
        << std::endl;

    float fi_number = expenses * (100.0f / scenario.wr);

    size_t months = 0;
    if (nw < fi_number && !income) {
        months = 12 * 1000;
    } else {
        while (nw < fi_number && months < 1200) {
            nw *= 1.0f + (returns / 100.0f) / 12.0f;
            nw += (income * sr / 100.0f) / 12.0f;
            ++months;
        }
    }

    // For now cannot be configured
    scenario.withdraw_frequency = 12;
    scenario.threshold          = 0.0f;
    scenario.start_year         = 1871;
    scenario.end_year           = 2022;

    auto portfolio_100   = swr::parse_portfolio("us_stocks:100;");
    auto values_100      = swr::load_values(portfolio_100);

    auto portfolio_60   = swr::parse_portfolio("us_stocks:60;us_bonds:40;");
    auto values_60      = swr::load_values(portfolio_60);

    auto portfolio_40   = swr::parse_portfolio("us_stocks:40;us_bonds:60;");
    auto values40      = swr::load_values(portfolio_40);

    scenario.inflation_data = swr::load_inflation(values_100, "us_inflation");

    scenario.portfolio  = portfolio_100;
    scenario.values     = values_100;
    scenario.years              = 30;
    auto results_30_100 = simulation(scenario);
    scenario.years              = 40;
    auto results_40_100 = simulation(scenario);
    scenario.years              = 50;
    auto results_50_100 = simulation(scenario);

    scenario.portfolio = portfolio_60;
    scenario.values    = values_60;
    scenario.years              = 30;
    auto results_30_60 = simulation(scenario);
    scenario.years              = 40;
    auto results_40_60 = simulation(scenario);
    scenario.years              = 50;
    auto results_50_60 = simulation(scenario);

    scenario.portfolio = portfolio_40;
    scenario.values    = values40;
    scenario.years              = 30;
    auto results_30_40 = simulation(scenario);
    scenario.years              = 40;
    auto results_40_40 = simulation(scenario);
    scenario.years              = 50;
    auto results_50_40 = simulation(scenario);

    std::stringstream ss;

    ss << "{ \"results\": {\n"
       << "  \"message\": \"\",\n"
       << "  \"error\": false,\n"
       << "  \"fi_number\": " << std::setprecision(2) << std::fixed << fi_number << ",\n"
       << "  \"years\": " << months / 12 << ",\n"
       << "  \"months\": " << months % 12 << ",\n"
       << "  \"success_rate_100\": " << results_30_100.success_rate << ",\n"
       << "  \"success_rate_60\": "  << results_30_60.success_rate << ",\n"
       << "  \"success_rate_40\": "  << results_30_40.success_rate << ",\n"
       << "  \"success_rate40_100\": " << results_40_100.success_rate << ",\n"
       << "  \"success_rate40_60\": "  << results_40_60.success_rate << ",\n"
       << "  \"success_rate40_40\": "  << results_40_40.success_rate << ",\n"
       << "  \"success_rate50_100\": " << results_50_100.success_rate << ",\n"
       << "  \"success_rate50_60\": "  << results_50_60.success_rate << ",\n"
       << "  \"success_rate50_40\": "  << results_50_40.success_rate << "\n"
       << "}}";

    res.set_content(ss.str(), "text/json");

    auto stop = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(stop - start).count();
    std::cout << "DEBUG: Simulated in " << duration << "ms" << std::endl;
}

} // namespace

int main(int argc, const char* argv[]) {
    auto args = parse_args(argc, argv);

    if (args.empty()) {
        std::cout << "Not enough arguments" << std::endl;
        return 0;
    } else {
        const auto & command = args[0];

        if (command == "fixed") {
            if (args.size() < 7) {
                std::cout << "Not enough arguments for fixed" << std::endl;
                return 1;
            }

            swr::scenario scenario;

            scenario.wr         = atof(args[1].c_str());
            scenario.years      = atoi(args[2].c_str());
            scenario.start_year = atoi(args[3].c_str());
            scenario.end_year   = atoi(args[4].c_str());
            scenario.portfolio  = swr::parse_portfolio(args[5]);
            auto inflation      = args[6];

            if (args.size() > 7) {
                scenario.fees = atof(args[7].c_str()) / 100.0f;
            }

            if (args.size() > 8) {
                scenario.final_threshold = atof(args[8].c_str()) / 100.0f;
            }

            swr::normalize_portfolio(scenario.portfolio);

            scenario.values         = swr::load_values(scenario.portfolio);
            scenario.inflation_data = swr::load_inflation(scenario.values, inflation);

            std::cout << "Withdrawal Rate (WR): " << scenario.wr << "%\n"
                      << "     Number of years: " << scenario.years << "\n"
                      << "               Start: " << scenario.start_year << "\n"
                      << "                 End: " << scenario.end_year << "\n"
                      << "                 TER: " << 100.0f * scenario.fees << "%\n"
                      << "           Portfolio: \n";

            for (auto & position : scenario.portfolio) {
                std::cout << "             " << position.asset << ": " << position.allocation << "%\n";
            }

            auto printer = [scenario](const std::string& message, const auto & results) {
                std::cout << "     Success Rate (" << message << "): (" << results.successes << "/" << (results.failures + results.successes) << ") " << results.success_rate
                          << " [" << results.tv_average << ":" << results.tv_median << ":" << results.tv_minimum << ":" << results.tv_maximum << "]" << std::endl;

                if (results.failures) {
                    std::cout << "         Worst duration: " << results.worst_duration << " months (" << results.worst_starting_month << "/" << results.worst_starting_year << ")" << std::endl;
                } else {
                    std::cout << "         Worst duration: " << scenario.years * 12 << " months" << std::endl;
                }

                std::cout << "         Worst result: " << results.worst_tv << " (" << results.worst_tv_month << "/" << results.worst_tv_year << ")" << std::endl;
                std::cout << "          Best result: " << results.best_tv << " (" << results.best_tv_month << "/" << results.best_tv_year << ")" << std::endl;

                std::cout << "         Highest Eff. WR: " << results.highest_eff_wr << "% ("
                          << results.highest_eff_wr_start_month << "/" << results.highest_eff_wr_start_year
                          << "->" << results.highest_eff_wr_year << ")" << std::endl;
                std::cout << "          Lowest Eff. WR: " << results.lowest_eff_wr << "% ("
                          << results.lowest_eff_wr_start_month << "/" << results.lowest_eff_wr_start_year
                          << "->" << results.lowest_eff_wr_year << ")" << std::endl;

            };

            auto start = std::chrono::high_resolution_clock::now();

            scenario.withdraw_frequency = 12;
            auto yearly_results = swr::simulation(scenario);

            if (yearly_results.message.size()) {
                std::cout << yearly_results.message << std::endl;
            }

            if (yearly_results.error) {
                return 1;
            }

            printer("Yearly", yearly_results);

            scenario.withdraw_frequency = 1;
            auto monthly_results = swr::simulation(scenario);

            printer("Monthly", monthly_results);

            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

            std::cout << "Computed " << swr::simulations_ran() << " withdrawal rates in " << duration << "ms ("
                      << 1000 * (swr::simulations_ran() / duration) << "/s)" << std::endl;
        } else if (command == "swr") {
            if (args.size() < 6) {
                std::cout << "Not enough arguments for swr" << std::endl;
                return 1;
            }

            swr::scenario scenario;

            scenario.years      = atoi(args[1].c_str());
            scenario.start_year = atoi(args[2].c_str());
            scenario.end_year   = atoi(args[3].c_str());
            scenario.portfolio  = swr::parse_portfolio(args[4]);
            auto inflation      = args[5];

            if (args.size() > 6) {
                scenario.fees = atof(args[6].c_str()) / 100.0f;
            }

            float limit = 95.0f;
            if (args.size() > 7) {
                limit = atof(args[7].c_str());
            }

            swr::normalize_portfolio(scenario.portfolio);

            scenario.values         = swr::load_values(scenario.portfolio);
            scenario.inflation_data = swr::load_inflation(scenario.values, inflation);

            for (auto & position : scenario.portfolio) {
                std::cout << "             " << position.asset << ": " << position.allocation << "%\n";
            }

            auto start = std::chrono::high_resolution_clock::now();

            scenario.withdraw_frequency = 1;

            float best_wr = 0.0f;
            swr::results best_results;

            for (float wr = 6.0f; wr >= 2.0f; wr -= 0.01f) {
                scenario.wr = wr;

                auto results = swr::simulation(scenario);

                if (results.message.size()) {
                    std::cout << results.message << std::endl;
                }

                if (results.error) {
                    return 1;
                }

                if (results.success_rate > limit) {
                    best_results = results;
                    best_wr = wr;
                    break;
                }
            }

            std::cout << "WR: " << best_wr << "(" << best_results.success_rate << ")" << std::endl;

            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

            std::cout << "Computed " << swr::simulations_ran() << " withdrawal rates in " << duration << "ms ("
                      << 1000 * (swr::simulations_ran() / duration) << "/s)" << std::endl;
        } else if (command == "multiple_wr") {
            if (args.size() < 7) {
                std::cout << "Not enough arguments for multiple_wr" << std::endl;
                return 1;
            }

            swr::scenario scenario;

            scenario.years      = atoi(args[1].c_str());
            scenario.start_year = atoi(args[2].c_str());
            scenario.end_year   = atoi(args[3].c_str());
            scenario.portfolio  = swr::parse_portfolio(args[4]);
            auto inflation      = args[5];
            scenario.rebalance  = swr::parse_rebalance(args[6]);

            scenario.values         = swr::load_values(scenario.portfolio);
            scenario.inflation_data = swr::load_inflation(scenario.values, inflation);

            std::cout << "     Number of years: " << scenario.years << "\n"
                      << "           Rebalance: " << scenario.rebalance << "\n"
                      << "               Start: " << scenario.start_year << "\n"
                      << "                 End: " << scenario.end_year << "\n";

            auto start = std::chrono::high_resolution_clock::now();

            if (total_allocation(scenario.portfolio) == 0.0f) {
                if (scenario.portfolio.size() != 2) {
                    std::cout << "Portfolio allocation cannot be zero!" << std::endl;
                    return 1;
                }

                for (size_t i = 0; i <= 100; i += 5) {
                    scenario.portfolio[0].allocation = float(i);
                    scenario.portfolio[1].allocation = float(100 - i);

                    multiple_wr(scenario);
                }
            } else {
                swr::normalize_portfolio(scenario.portfolio);
                multiple_wr(scenario);
            }

            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

            std::cout << "Computed " << swr::simulations_ran() << " withdrawal rates in " << duration << "ms ("
                      << 1000 * (swr::simulations_ran() / duration) << "/s)" << std::endl;
        } else if (command == "withdraw_frequency") {
            if (args.size() < 7) {
                std::cout << "Not enough arguments for withdraw_frequency" << std::endl;
                return 1;
            }

            swr::scenario scenario;

            scenario.wr         = atof(args[1].c_str());
            scenario.years      = atoi(args[2].c_str());
            scenario.start_year = atoi(args[3].c_str());
            scenario.end_year   = atoi(args[4].c_str());
            scenario.portfolio  = swr::parse_portfolio(args[5]);
            auto inflation      = args[6];

            if (args.size() > 7) {
                scenario.fees = atof(args[7].c_str()) / 100.0f;
            }

            float portfolio_add = 20;
            if (args.size() > 8){
                portfolio_add = atof(args[8].c_str());
            }

            scenario.values         = swr::load_values(scenario.portfolio);
            scenario.inflation_data = swr::load_inflation(scenario.values, inflation);

            std::cout << "Withdrawal Rate (WR): " << scenario.wr << "%\n"
                      << "     Number of years: " << scenario.years << "\n"
                      << "               Start: " << scenario.start_year << "\n"
                      << "                 End: " << scenario.end_year << "\n"
                      << "                 TER: " << 100.0f * scenario.fees << "%\n";

            auto start = std::chrono::high_resolution_clock::now();

            std::cout << "portfolio;";
            for (size_t f = 1; f <= 24; ++f) {
                std::cout << f << ";";
            }
            std::cout << std::endl;

            if (total_allocation(scenario.portfolio) == 0.0f) {
                if (scenario.portfolio.size() != 2) {
                    std::cout << "Portfolio allocation cannot be zero!" << std::endl;
                    return 1;
                }

                for (size_t i = 0; i <= 100; i += portfolio_add) {
                    scenario.portfolio[0].allocation = float(i);
                    scenario.portfolio[1].allocation = float(100 - i);

                    for (auto& position : scenario.portfolio) {
                        if (position.allocation > 0) {
                            std::cout << position.allocation << "% " << position.asset << " ";
                        }
                    }

                    for (size_t f = 1; f <= 24; ++f) {
                        scenario.withdraw_frequency = f;

                        auto results = swr::simulation(scenario);

                        if (results.message.size()) {
                            std::cout << results.message << std::endl;
                        }

                        if (results.error) {
                            return 1;
                        }

                        std::cout << ";" << results.success_rate;
                    }

                    std::cout << std::endl;
                }
            } else {
                swr::normalize_portfolio(scenario.portfolio);

                for (auto& position : scenario.portfolio) {
                    if (position.allocation > 0) {
                        std::cout << position.allocation << "% " << position.asset << " ";
                    }
                }

                for (size_t f = 1; f <= 24; ++f) {
                    scenario.withdraw_frequency = f;

                    auto results = swr::simulation(scenario);

                    if (results.message.size()) {
                        std::cout << results.message << std::endl;
                    }

                    if (results.error) {
                        return 1;
                    }

                    std::cout << ";" << results.success_rate;
                }

                std::cout << std::endl;
                std::cout << std::endl;

                for (float w = 3.0f; w <= 6.0f; w += 0.25f) {
                    std::cout << w;

                    scenario.wr = w;

                    for (size_t f = 1; f <= 24; ++f) {
                        scenario.withdraw_frequency = f;

                        auto results = swr::simulation(scenario);

                        if (results.message.size()) {
                            std::cout << results.message << std::endl;
                        }

                        if (results.error) {
                            return 1;
                        }

                        std::cout << ";" << results.success_rate;
                    }

                    std::cout << std::endl;
                }
            }

            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

            std::cout << "Computed " << swr::simulations_ran() << " withdrawal rates in " << duration << "ms ("
                      << 1000 * (swr::simulations_ran() / duration) << "/s)" << std::endl;
        } else if (command == "frequency") {
            if (args.size() < 6) {
                std::cout << "Not enough arguments for frequency" << std::endl;
                return 1;
            }

            size_t start_year  = atoi(args[1].c_str());
            size_t end_year    = atoi(args[2].c_str());
            size_t years       = atoi(args[3].c_str());
            size_t frequency   = atoi(args[4].c_str());
            size_t monthly_buy = atoi(args[5].c_str());

            auto portfolio = swr::parse_portfolio("ch_stocks:100;");
            auto values    = swr::load_values(portfolio);

            const auto months = years * 12;

            std::vector<swr::data>::const_iterator returns;

            float total = 0;
            float max = 0;
            size_t simulations = 0;

            for (size_t current_year = start_year; current_year <= end_year - years; ++current_year) {
                for (size_t current_month = 1; current_month <= 12; ++current_month) {
                    size_t end_year  = current_year + (current_month - 1 + months - 1) / 12;
                    size_t end_month = 1 + ((current_month - 1) + (months - 1) % 12) % 12;

                    size_t months = 0;

                    returns = swr::get_start(values[0], current_year, (current_month % 12) + 1);

                    float net_worth = 0;

                    for (size_t y = current_year; y <= end_year; ++y) {
                        for (size_t m = (y == current_year ? current_month : 1); m <= (y == end_year ? end_month : 12); ++m, ++months) {
                            // Adjust the portfolio with the returns
                            net_worth *= returns->value;
                            ++returns;

                            if (months % frequency == frequency - 1) {
                                net_worth += frequency * monthly_buy;
                            }
                        }
                    }

                    total += net_worth;
                    ++simulations;

                    max = std::max(net_worth, max);
                }
            }

            std::array<float, 6> worst_results;
            std::array<float, 6> best_results;
            worst_results.fill(0.0f);
            best_results.fill(0.0f);

            for (size_t current_year = start_year; current_year <= end_year - years; ++current_year) {
                for (size_t current_month = 1; current_month <= 12; ++current_month) {
                    size_t end_year  = current_year + (current_month - 1 + months - 1) / 12;
                    size_t end_month = 1 + ((current_month - 1) + (months - 1) % 12) % 12;

                    std::array<float, 6> results;

                    for (size_t freq = 1; freq <= 6; ++freq) {
                        size_t months = 0;

                        returns = swr::get_start(values[0], current_year, (current_month % 12) + 1);

                        float net_worth = 0;

                        for (size_t y = current_year; y <= end_year; ++y) {
                            for (size_t m = (y == current_year ? current_month : 1); m <= (y == end_year ? end_month : 12); ++m, ++months) {
                                // Adjust the portfolio with the returns
                                net_worth *= returns->value;
                                ++returns;

                                if (months % freq == freq - 1) {
                                    net_worth += freq * monthly_buy;
                                }
                            }
                        }

                        results[freq - 1] = net_worth;
                    }

                    for (size_t f = 1; f < 6; ++f) {
                        worst_results[f] = std::max(worst_results[f], results[0] - results[f]);
                        best_results[f] = std::min(best_results[f], results[0] - results[f]);
                    }
                }
            }

            std::cout << "Average: " << std::fixed << total / simulations << std::endl;
            std::cout << "Max: " << std::fixed << max << std::endl;
            std::cout << "Simulations: " << simulations << std::endl;

            for (size_t f = 1; f < 6; ++f) {
                std::cout << "Worst case " << f+1 << " : " << worst_results[f] << std::endl;
            }

            for (size_t f = 1; f < 6; ++f) {
                std::cout << "Best case " << f+1 << " : " << best_results[f] << std::endl;
            }


        } else if (command == "analysis") {
            if (args.size() < 2) {
                std::cout << "Not enough arguments for analysis" << std::endl;
                return 1;
            }

            size_t start_year = atoi(args[1].c_str());
            size_t end_year   = atoi(args[2].c_str());

            auto portfolio = swr::parse_portfolio("us_stocks:50;us_bonds:50;");

            auto values         = swr::load_values(portfolio);
            auto inflation_data = swr::load_inflation(values, "us_inflation");

            auto analyzer = [&](auto & v, const std::string & name) {
                float average = 0.0f;

                float worst_month = 1.0f;
                std::string worst_month_str;

                float best_month = 0.0f;
                std::string best_month_str;

                size_t negative = 0;
                size_t total = 0;

                for (auto value : v) {
                    if (value.year >= start_year && value.year <= end_year) {
                        if (value.value < worst_month) {
                            worst_month = value.value;
                            worst_month_str = std::to_string(value.year) + "." + std::to_string(value.month);
                        }

                        if (value.value > best_month) {
                            best_month = value.value;
                            best_month_str = std::to_string(value.year) + "." + std::to_string(value.month);
                        }

                        ++total;

                        if (value.value < 1.0f) {
                            ++negative;
                        }

                        average += value.value;
                    }
                }

                std::cout << name << " average returns: +" << 100.0f * ((average / total) - 1.0f) << "%" << std::endl;
                std::cout << name << " best returns: +" << 100.0f * (best_month - 1.0f) << "% (" << best_month_str << ")" << std::endl;
                std::cout << name << " worst returns: -" << 100.0f * (1.0f - worst_month) << "% (" << worst_month_str << ")" << std::endl;
                std::cout << name << " Negative months: " << negative << " (" << 100.0f * (negative / float(total)) << "%)" << std::endl;
            };

            analyzer(values[0], "Stocks");
            analyzer(values[1], "Bonds");
            analyzer(inflation_data, "Inflation");
        } else if (command == "glidepath" || command == "reverse_glidepath") {
            swr::scenario scenario;

            scenario.years      = atoi(args[1].c_str());
            scenario.start_year = atoi(args[2].c_str());
            scenario.end_year   = atoi(args[3].c_str());
            scenario.portfolio  = swr::parse_portfolio(args[4]);
            auto inflation      = args[5];
            scenario.rebalance  = swr::parse_rebalance(args[6]);

            std::cout << scenario.rebalance << std::endl;

            float start_wr = 3.0f;
            if (args.size() > 7) {
                start_wr = atof(args[7].c_str());
            }

            float end_wr   = 6.0f;
            if (args.size() > 8) {
                end_wr = atof(args[8].c_str());
            }

            float add_wr   = 0.1f;
            if (args.size() > 9) {
                add_wr = atof(args[9].c_str());
            }

            swr::normalize_portfolio(scenario.portfolio);
            scenario.values         = swr::load_values(scenario.portfolio);
            scenario.inflation_data = swr::load_inflation(scenario.values, inflation);

            std::stringstream failsafe_ss;

            auto failsafe_and_success = [&](auto title) {
                multiple_wr_success_sheets(title, scenario, start_wr, end_wr, add_wr);
                failsafe_swr(title, scenario, 6.0f, 0.0f, 0.01f, failsafe_ss);
            };

            if (command == "glidepath") {
                scenario.glidepath = false;
                scenario.portfolio[0].allocation = 40;
                scenario.portfolio[1].allocation = 60;
                failsafe_and_success("Static 40%");

                scenario.glidepath = true;
                scenario.gp_goal = 80.0f;

                scenario.gp_pass = 0.2;
                failsafe_and_success("40%->80%,+0.2");

                scenario.gp_pass = 0.3;
                failsafe_and_success("40%->80%,+0.3");

                scenario.gp_pass = 0.4;
                failsafe_and_success("40%->80%,+0.4");

                scenario.gp_pass = 0.5;
                failsafe_and_success("40%->80%,+0.5");

                scenario.glidepath = true;
                scenario.gp_goal = 100.0f;

                scenario.gp_pass = 0.2;
                failsafe_and_success("40%->100%,0.2");

                scenario.gp_pass = 0.3;
                failsafe_and_success("40%->100%,+0.3");

                scenario.gp_pass = 0.4;
                failsafe_and_success("40%->100%,+0.4");

                scenario.gp_pass = 0.5;
                failsafe_and_success("40%->100%,+0.5");

                scenario.glidepath = false;
                scenario.portfolio[0].allocation = 60;
                scenario.portfolio[1].allocation = 40;
                failsafe_and_success("Static 60%");

                scenario.glidepath = true;
                scenario.gp_goal = 80.0f;

                scenario.gp_pass = 0.2;
                failsafe_and_success("60%->80%,+0.2");

                scenario.gp_pass = 0.3;
                failsafe_and_success("60%->80%,+0.3");

                scenario.gp_pass = 0.4;
                failsafe_and_success("60%->80%,+0.4");

                scenario.gp_pass = 0.5;
                failsafe_and_success("60%->80%,+0.5");

                scenario.glidepath = true;
                scenario.gp_goal = 100.0f;

                scenario.gp_pass = 0.2;
                failsafe_and_success("60%->100%,+0.2");

                scenario.gp_pass = 0.3;
                failsafe_and_success("60%->100%,+0.3");

                scenario.gp_pass = 0.4;
                failsafe_and_success("60%->100%,+0.4");

                scenario.gp_pass = 0.5;
                failsafe_and_success("60%->100%,+0.5");

                scenario.glidepath = false;
                scenario.portfolio[0].allocation = 80;
                scenario.portfolio[1].allocation = 20;
                failsafe_and_success("Static 80%");

                scenario.glidepath = true;
                scenario.gp_goal = 100.0f;

                scenario.gp_pass = 0.2;
                failsafe_and_success("80%->100%,+0.2");

                scenario.gp_pass = 0.3;
                failsafe_and_success("80%->100%,+0.3");

                scenario.gp_pass = 0.4;
                failsafe_and_success("80%->100%,+0.4");

                scenario.gp_pass = 0.5;
                failsafe_and_success("80%->100%,+0.5");

                scenario.glidepath = false;
                scenario.portfolio[0].allocation = 100;
                scenario.portfolio[1].allocation = 0;
                failsafe_and_success("Static 100%");
            } else {
                scenario.glidepath = false;
                scenario.portfolio[0].allocation = 100;
                scenario.portfolio[1].allocation = 0;
                failsafe_and_success("Static 100%");

                scenario.glidepath = true;
                scenario.gp_goal = 80.0f;

                scenario.gp_pass = -0.2;
                failsafe_and_success("100%->80%,-0.2");

                scenario.gp_pass = -0.3;
                failsafe_and_success("100%->80%,-0.3");

                scenario.gp_pass = -0.4;
                failsafe_and_success("100%->80%,-0.4");

                scenario.gp_pass = -0.5;
                failsafe_and_success("100%->80%,-0.5");
            }

            std::cout << std::endl;
            std::cout << "Portfolio;Failsafe;1%;5%;10%;25%\n";
            std::cout << failsafe_ss.str();
        } else if (command == "failsafe") {
            swr::scenario scenario;

            scenario.years      = atoi(args[1].c_str());
            scenario.start_year = atoi(args[2].c_str());
            scenario.end_year   = atoi(args[3].c_str());
            scenario.portfolio  = swr::parse_portfolio(args[4]);
            auto inflation      = args[5];
            scenario.rebalance  = swr::parse_rebalance(args[6]);

            float portfolio_add = 10;
            if (args.size() > 7){
                portfolio_add = atof(args[7].c_str());
            }

            scenario.values         = swr::load_values(scenario.portfolio);
            scenario.inflation_data = swr::load_inflation(scenario.values, inflation);

            if (total_allocation(scenario.portfolio) == 0.0f) {
                if (scenario.portfolio.size() != 2) {
                    std::cout << "Portfolio allocation cannot be zero!" << std::endl;
                    return 1;
                }

                std::cout << "Portfolio;Failsafe;1%;5%;10%;25%\n";

                for (size_t i = 0; i <= 100; i += portfolio_add) {
                    scenario.portfolio[0].allocation = float(i);
                    scenario.portfolio[1].allocation = float(100 - i);

                    failsafe_swr("", scenario, 6.0f, 0.0f, 0.01f, std::cout);
                }
            } else {
                std::cout << "Portfolio;Failsafe;1%;5%;10%;25%\n";
                swr::normalize_portfolio(scenario.portfolio);
                failsafe_swr("", scenario, 6.0f, 0.0f, 0.01f, std::cout);
            }
        } else if (command == "trinity_success_sheets" || command == "trinity_success_graph") {
            if (args.size() < 7) {
                std::cout << "Not enough arguments for trinity_sheets" << std::endl;
                return 1;
            }

            const bool graph = command == "trinity_success_graph";

            swr::scenario scenario;

            scenario.years      = atoi(args[1].c_str());
            scenario.start_year = atoi(args[2].c_str());
            scenario.end_year   = atoi(args[3].c_str());
            scenario.portfolio  = swr::parse_portfolio(args[4]);
            auto inflation      = args[5];
            scenario.rebalance  = swr::parse_rebalance(args[6]);

            float portfolio_add = 25;
            if (args.size() > 7){
                portfolio_add = atof(args[7].c_str());
            }

            float start_wr = 3.0f;
            if (args.size() > 8) {
                start_wr = atof(args[8].c_str());
            }

            float end_wr   = 6.0f;
            if (args.size() > 9) {
                end_wr = atof(args[9].c_str());
            }

            float add_wr   = 0.1f;
            if (args.size() > 10) {
                add_wr = atof(args[10].c_str());
            }

            if (args.size() > 11) {
                scenario.fees = atof(args[11].c_str()) / 100.0f;
            }

            if (args.size() > 12) {
                scenario.final_threshold = atof(args[12].c_str()) / 100.0f;
            }

            if (args.size() > 13) {
                scenario.final_inflation = args[13] == "true";
            }

            scenario.values         = swr::load_values(scenario.portfolio);
            scenario.inflation_data = swr::load_inflation(scenario.values, inflation);

            if (args.size() > 14) {
                std::string country = args[14];

                if (country == "switzerland") {
                    auto exchange_data = swr::load_exchange("usd_chf");

                    scenario.exchanges.resize(scenario.values.size());

                    for (size_t i = 0; i < scenario.portfolio.size(); ++i) {
                        scenario.exchanges[i] = exchange_data;
                        if (scenario.portfolio[i].asset == "us_stocks") {
                            scenario.exchanges[i] = exchange_data;
                        } else {
                            scenario.exchanges[i] = exchange_data;

                            for (auto& v : scenario.exchanges[i]) {
                                v.value = 1;
                            }
                        }
                    }
                } else {
                    std::cout << "No support for country: " << country << std::endl;
                    return 1;
                }
            }

            Graph g(graph);

            if (!graph) {
                std::cout << "Portfolio";
                for (float wr = start_wr; wr < end_wr + add_wr / 2.0f; wr += add_wr) {
                    std::cout << ";" << wr << "%";
                }
                std::cout << "\n";
            }

            if (total_allocation(scenario.portfolio) == 0.0f) {
                if (scenario.portfolio.size() != 2) {
                    std::cout << "Portfolio allocation cannot be zero!" << std::endl;
                    return 1;
                }

                for (size_t i = 0; i <= 100; i += portfolio_add) {
                    scenario.portfolio[0].allocation = float(i);
                    scenario.portfolio[1].allocation = float(100 - i);

                    if (graph) {
                        multiple_wr_success_graph(g, "", scenario, start_wr, end_wr, add_wr);
                    } else {
                        multiple_wr_success_sheets("", scenario, start_wr, end_wr, add_wr);
                    }
                }
            } else {
                swr::normalize_portfolio(scenario.portfolio);
                if (graph) {
                    multiple_wr_success_graph(g, "", scenario, start_wr, end_wr, add_wr);
                } else {
                    multiple_wr_success_sheets("", scenario, start_wr, end_wr, add_wr);
                }
            }
        } else if (command == "trinity_duration_sheets") {
            swr::scenario scenario;

            scenario.years      = atoi(args[1].c_str());
            scenario.start_year = atoi(args[2].c_str());
            scenario.end_year   = atoi(args[3].c_str());
            scenario.portfolio  = swr::parse_portfolio(args[4]);
            auto inflation      = args[5];
            scenario.rebalance  = swr::parse_rebalance(args[6]);

            scenario.fees = 0.0f;
            if (args.size() > 7) {
                scenario.fees = atof(args[7].c_str()) / 100.0f;
            }

            const float start_wr = 3.0f;
            const float end_wr   = 5.0f;
            const float add_wr   = 0.1f;

            const float portfolio_add = 20;

            scenario.values         = swr::load_values(scenario.portfolio);
            scenario.inflation_data = swr::load_inflation(scenario.values, inflation);

            std::cout << "Duration";

            for (size_t i = 0; i <= 100; i += portfolio_add) {
                scenario.portfolio[0].allocation = float(i);
                scenario.portfolio[1].allocation = float(100 - i);

                std::cout << ";";
                for (auto& position : scenario.portfolio) {
                    if (position.allocation > 0) {
                        std::cout << position.allocation << "% " << position.asset << " ";
                    }
                }
            }

            std::cout << std::endl;

            for (float wr = start_wr; wr < end_wr + add_wr / 2.0f; wr += add_wr) {
                std::cout << wr;

                scenario.wr = wr;

                for (size_t i = 0; i <= 100; i += portfolio_add) {
                    scenario.portfolio[0].allocation = float(i);
                    scenario.portfolio[1].allocation = float(100 - i);

                    auto monthly_results = swr::simulation(scenario);

                    if (monthly_results.worst_duration == 0) {
                        std::cout << ";" << scenario.years * 12;
                    } else {
                        std::cout << ";" << monthly_results.worst_duration;
                    }
                }

                std::cout << std::endl;
            }

            std::cout << std::endl;
            std::cout << std::endl;

            for (float wr = start_wr; wr < end_wr + add_wr / 2.0f; wr += add_wr) {
                std::cout << wr;

                scenario.wr = wr;

                for (size_t i = 0; i <= 100; i += portfolio_add) {
                    scenario.portfolio[0].allocation = float(i);
                    scenario.portfolio[1].allocation = float(100 - i);

                    auto monthly_results = swr::simulation(scenario);
                    std::cout << ";" << monthly_results.success_rate;
                }

                std::cout << std::endl;
            }
        } else if (command == "trinity_tv_sheets") {
            if (args.size() < 7) {
                std::cout << "Not enough arguments for trinity_sheets" << std::endl;
                return 1;
            }

            swr::scenario scenario;

            scenario.years      = atoi(args[1].c_str());
            scenario.start_year = atoi(args[2].c_str());
            scenario.end_year   = atoi(args[3].c_str());
            scenario.portfolio  = swr::parse_portfolio(args[4]);
            auto inflation      = args[5];
            scenario.rebalance  = swr::parse_rebalance(args[6]);

            scenario.fees = 0.0f;
            if (args.size() > 7) {
                scenario.fees = atof(args[7].c_str()) / 100.0f;
            }

            const float start_wr = 3.0f;
            const float end_wr   = 5.0f;
            const float add_wr   = 0.25f;

            scenario.values         = swr::load_values(scenario.portfolio);
            scenario.inflation_data = swr::load_inflation(scenario.values, inflation);

            std::cout << "Withdrawal Rate";
            for (float wr = start_wr; wr < end_wr + add_wr / 2.0f; wr += add_wr) {
                std::cout << ";" << wr << "%";
            }
            std::cout << "\n";

            swr::normalize_portfolio(scenario.portfolio);
            multiple_wr_tv_sheets(scenario, start_wr, end_wr, add_wr);
        } else if (command == "current_wr") {
            if (args.size() < 7) {
                std::cout << "Not enough arguments for current_wr" << std::endl;
                return 1;
            }

            swr::scenario scenario;

            scenario.method     = swr::Method::CURRENT;
            scenario.years      = atoi(args[1].c_str());
            scenario.start_year = atoi(args[2].c_str());
            scenario.end_year   = atoi(args[3].c_str());
            scenario.portfolio  = swr::parse_portfolio(args[4]);
            auto inflation      = args[5];
            scenario.rebalance  = swr::parse_rebalance(args[6]);
            scenario.fees       = 0.001; // TER = 0.1%

            float portfolio_add = 25;

            float start_wr = 3.0f;
            float end_wr   = 6.0f;
            float add_wr   = 0.1f;

            if (args.size() > 7){
                portfolio_add = atof(args[7].c_str());
            }

            if (args.size() > 8) {
                start_wr = atof(args[8].c_str());
            }

            if (args.size() > 9) {
                end_wr = atof(args[9].c_str());
            }

            if (args.size() > 10) {
                add_wr = atof(args[10].c_str());
            }

            if (args.size() > 11) {
                scenario.minimum = atoi(args[11].c_str());
            }

            if (args.size() > 12 && args[12] == "standard") {
                scenario.method = swr::Method::STANDARD;
            }

            scenario.values         = swr::load_values(scenario.portfolio);
            scenario.inflation_data = swr::load_inflation(scenario.values, inflation);

            std::cout << "Portfolio";
            for (float wr = start_wr; wr < end_wr + add_wr / 2.0f; wr += add_wr) {
                std::cout << ";" << wr << "%";
            }
            std::cout << "\n";

            if (total_allocation(scenario.portfolio) == 0.0f) {
                if (scenario.portfolio.size() != 2) {
                    std::cout << "Portfolio allocation cannot be zero!" << std::endl;
                    return 1;
                }

                for (size_t i = 0; i <= 100; i += portfolio_add) {
                    scenario.portfolio[0].allocation = float(i);
                    scenario.portfolio[1].allocation = float(100 - i);

                    multiple_wr_success_sheets("", scenario, start_wr, end_wr, add_wr);
                }

                std::cout << '\n';

                for (size_t i = 0; i <= 100; i += portfolio_add) {
                    scenario.portfolio[0].allocation = float(i);
                    scenario.portfolio[1].allocation = float(100 - i);

                    multiple_wr_withdrawn_sheets("", scenario, start_wr, end_wr, add_wr);
                }

                std::cout << '\n';

                for (size_t i = 0; i <= 100; i += portfolio_add) {
                    scenario.portfolio[0].allocation = float(i);
                    scenario.portfolio[1].allocation = float(100 - i);

                    multiple_wr_duration_sheets("", scenario, start_wr, end_wr, add_wr);
                }
            } else {
                swr::normalize_portfolio(scenario.portfolio);
                multiple_wr_success_sheets("", scenario, start_wr, end_wr, add_wr);
            }

            std::cout << "\n";
        } else if (command == "rebalance_sheets") {
            if (args.size() < 6) {
                std::cout << "Not enough arguments for rebalance_sheets" << std::endl;
                return 1;
            }

            swr::scenario scenario;

            scenario.years      = atoi(args[1].c_str());
            scenario.start_year = atoi(args[2].c_str());
            scenario.end_year   = atoi(args[3].c_str());
            scenario.portfolio    = swr::parse_portfolio(args[4]);
            auto inflation        = args[5];

            const float start_wr = 3.0f;
            const float end_wr   = 6.0f;
            const float add_wr   = 0.1f;

            scenario.values         = swr::load_values(scenario.portfolio);
            scenario.inflation_data = swr::load_inflation(scenario.values, inflation);

            std::cout << "Rebalance";
            for (float wr = start_wr; wr < end_wr + add_wr / 2.0f; wr += add_wr) {
                std::cout << ";" << wr << "%";
            }
            std::cout << "\n";

            auto start = std::chrono::high_resolution_clock::now();

            swr::normalize_portfolio(scenario.portfolio);

            scenario.rebalance = swr::Rebalancing::NONE;
            multiple_rebalance_sheets(scenario, start_wr, end_wr, add_wr);

            scenario.rebalance = swr::Rebalancing::MONTHLY;
            multiple_rebalance_sheets(scenario, start_wr, end_wr, add_wr);

            scenario.rebalance = swr::Rebalancing::YEARLY;
            multiple_rebalance_sheets(scenario, start_wr, end_wr, add_wr);

            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

            std::cout << "Computed " << swr::simulations_ran() << " withdrawal rates in " << duration << "ms ("
                      << 1000 * (swr::simulations_ran() / duration) << "/s)" << std::endl;
        } else if (command == "threshold_rebalance_sheets") {
            if (args.size() < 6) {
                std::cout << "Not enough arguments for threshold_rebalance_sheets" << std::endl;
                return 1;
            }

            swr::scenario scenario;

            scenario.years      = atoi(args[1].c_str());
            scenario.start_year = atoi(args[2].c_str());
            scenario.end_year   = atoi(args[3].c_str());
            scenario.portfolio  = swr::parse_portfolio(args[4]);
            auto inflation      = args[5];

            const float start_wr = 3.0f;
            const float end_wr   = 6.0f;
            const float add_wr   = 0.1f;

            scenario.values         = swr::load_values(scenario.portfolio);
            scenario.inflation_data = swr::load_inflation(scenario.values, inflation);

            std::cout << "Rebalance";
            for (float wr = start_wr; wr < end_wr + add_wr / 2.0f; wr += add_wr) {
                std::cout << ";" << wr << "%";
            }
            std::cout << "\n";

            auto start = std::chrono::high_resolution_clock::now();

            swr::normalize_portfolio(scenario.portfolio);

            scenario.rebalance = swr::Rebalancing::THRESHOLD;

            scenario.threshold = 0.01;
            multiple_rebalance_sheets(scenario, start_wr, end_wr, add_wr);

            scenario.threshold = 0.02;
            multiple_rebalance_sheets(scenario, start_wr, end_wr, add_wr);

            scenario.threshold = 0.05;
            multiple_rebalance_sheets(scenario, start_wr, end_wr, add_wr);

            scenario.threshold = 0.10;
            multiple_rebalance_sheets(scenario, start_wr, end_wr, add_wr);

            scenario.threshold = 0.25;
            multiple_rebalance_sheets(scenario, start_wr, end_wr, add_wr);

            scenario.threshold = 0.50;
            multiple_rebalance_sheets(scenario, start_wr, end_wr, add_wr);

            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

            std::cout << "Computed " << swr::simulations_ran() << " withdrawal rates in " << duration << "ms ("
                      << 1000 * (swr::simulations_ran() / duration) << "/s)" << std::endl;
        } else if (command == "trinity_low_yield_sheets") {
            if (args.size() < 8) {
                std::cout << "Not enough arguments for trinity_low_yield_sheets" << std::endl;
                return 1;
            }

            swr::scenario scenario;

            scenario.years      = atoi(args[1].c_str());
            scenario.start_year = atoi(args[2].c_str());
            scenario.end_year   = atoi(args[3].c_str());
            scenario.portfolio  = swr::parse_portfolio(args[4]);
            auto inflation      = args[5];
            scenario.rebalance  = swr::parse_rebalance(args[6]);
            float yield_adjust  = atof(args[7].c_str());

            const float start_wr = 3.0f;
            const float end_wr   = 6.0f;
            const float add_wr   = 0.1f;

            const float portfolio_add = 10;

            scenario.values         = swr::load_values(scenario.portfolio);
            scenario.inflation_data = swr::load_inflation(scenario.values, inflation);

            std::cout << "Portfolio";
            for (float wr = start_wr; wr < end_wr + add_wr / 2.0f; wr += add_wr) {
                std::cout << ";" << wr << "%";
            }
            std::cout << "\n";

            for (size_t i = 0; i < scenario.portfolio.size(); ++i) {
                if (scenario.portfolio[i].asset == "us_bonds") {
                    for (auto & value : scenario.values[i]) {
                        value.value = value.value - ((value.value - 1.0f) * yield_adjust);
                    }

                    break;
                }
            }

            auto start = std::chrono::high_resolution_clock::now();

            if (total_allocation(scenario.portfolio) == 0.0f) {
                if (scenario.portfolio.size() != 2) {
                    std::cout << "Portfolio allocation cannot be zero!" << std::endl;
                    return 1;
                }

                for (size_t i = 0; i <= 100; i += portfolio_add) {
                    scenario.portfolio[0].allocation = float(i);
                    scenario.portfolio[1].allocation = float(100 - i);

                    multiple_wr_success_sheets("", scenario, start_wr, end_wr, add_wr);
                }
            } else {
                swr::normalize_portfolio(scenario.portfolio);
                multiple_wr_success_sheets("", scenario, start_wr, end_wr, add_wr);
            }

            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

            std::cout << "Computed " << swr::simulations_ran() << " withdrawal rates in " << duration << "ms ("
                      << 1000 * (swr::simulations_ran() / duration) << "/s)" << std::endl;
        } else if (command == "trinity_cash") {
            if (args.size() < 7) {
                std::cout << "Not enough arguments for trinity_cash" << std::endl;
                return 1;
            }

            swr::scenario scenario;

            scenario.years      = atoi(args[1].c_str());
            scenario.start_year = atoi(args[2].c_str());
            scenario.end_year   = atoi(args[3].c_str());
            scenario.portfolio  = swr::parse_portfolio(args[4]);
            auto inflation      = args[5];
            scenario.rebalance  = swr::parse_rebalance(args[6]);

            float portfolio_add = 25;
            if (args.size() > 7){
                portfolio_add = atof(args[7].c_str());
            }

            float wr = 4.0f;
            if (args.size() > 8){
                wr = atof(args[8].c_str());
            }

            scenario.cash_simple = true;
            if (args.size() > 9){
                scenario.cash_simple = args[9] == "true";
            }

            bool compare = false;
            if (args.size() > 10){
                compare = args[10] == "true";
            }

            scenario.values         = swr::load_values(scenario.portfolio);
            scenario.inflation_data = swr::load_inflation(scenario.values, inflation);

            auto start = std::chrono::high_resolution_clock::now();

            if (total_allocation(scenario.portfolio) == 0.0f) {
                if (scenario.portfolio.size() != 2) {
                    std::cout << "Portfolio allocation cannot be zero!" << std::endl;
                    return 1;
                }

                std::cout << "Portfolio";

                for (size_t i = 0; i <= 100; i += portfolio_add) {
                    scenario.portfolio[0].allocation = float(i);
                    scenario.portfolio[1].allocation = float(100 - i);

                    std::cout << ";";

                    for (auto& position : scenario.portfolio) {
                        if (position.allocation > 0) {
                            std::cout << position.allocation << "% " << position.asset << " ";
                        }
                    }
                }

                if (compare) {
                    for (size_t i = 0; i <= 100; i += portfolio_add) {
                        scenario.portfolio[0].allocation = float(i);
                        scenario.portfolio[1].allocation = float(100 - i);

                        std::cout << ";";

                        for (auto& position : scenario.portfolio) {
                            if (position.allocation > 0) {
                                std::cout << position.allocation << "% " << position.asset << " ";
                            }
                        }
                    }
                }

                std::cout << "\n";

                const float withdrawal = (wr / 100.0f) * swr::initial_value;

                std::array<std::vector<swr::results>, 61> all_results;
                std::array<std::vector<swr::results>, 61> all_compare_results;

                cpp::default_thread_pool pool(std::thread::hardware_concurrency());

                for (size_t M = 0; M <= 60; ++M) {
                    pool.do_task(
                            [&](size_t m) {
                                auto my_scenario = scenario;

                                my_scenario.wr           = wr;
                                my_scenario.initial_cash = m * ((swr::initial_value * (my_scenario.wr / 100.0f)) / 12);

                                for (size_t i = 0; i <= 100; i += portfolio_add) {
                                    my_scenario.portfolio[0].allocation = float(i);
                                    my_scenario.portfolio[1].allocation = float(100 - i);

                                    all_results[m].push_back(swr::simulation(my_scenario));
                                }

                                if (compare) {
                                    float total              = swr::initial_value + m * (withdrawal / 12.0f);
                                    my_scenario.wr           = 100.0f * (withdrawal / total);
                                    my_scenario.initial_cash = 0;

                                    for (size_t i = 0; i <= 100; i += portfolio_add) {
                                        my_scenario.portfolio[0].allocation = float(i);
                                        my_scenario.portfolio[1].allocation = float(100 - i);

                                        all_compare_results[m].push_back(swr::simulation(my_scenario));
                                    }
                                }
                            },
                            M);
                }

                pool.wait();

                for (size_t m = 0; m <= 60; ++m) {
                    std::cout << m;

                    for (auto& results : all_results[m]) {
                        std::cout << ';' << results.success_rate;
                    }

                    if (compare) {
                        for (auto& results : all_compare_results[m]) {
                            std::cout << ';' << results.success_rate;
                        }
                    }

                    std::cout << "\n";
                }

            } else {
                swr::normalize_portfolio(scenario.portfolio);

                std::cout << "Portfolio; ";

                for (auto& position : scenario.portfolio) {
                    if (position.allocation > 0) {
                        std::cout << position.allocation << "% " << position.asset << " ";
                    }
                }

                std::cout << "\n";

                for (size_t m = 0; m <= 60; ++m) {
                    scenario.initial_cash = m * ((swr::initial_value * (scenario.wr / 100.0f)) / 12);
                    auto results = swr::simulation(scenario);
                    std::cout << m << ';' << results.success_rate;
                    std::cout << "\n";
                }
            }

            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

            std::cout << "Computed " << swr::simulations_ran() << " withdrawal rates in " << duration << "ms ("
                      << 1000 * (swr::simulations_ran() / duration) << "/s)" << std::endl;
        } else if (command == "server") {
            if (args.size() < 3) {
                std::cout << "Not enough arguments for server" << std::endl;
                return 1;
            }

            std::string listen = args[1];
            auto port          = atoi(args[2].c_str());

            httplib::Server server;

            server.Get("/api/simple", &server_simple_api);
            server.Get("/api/retirement", &server_retirement_api);

            install_signal_handler();

            server_ptr = &server;
            std::cout << "Server is starting to listen on " << listen << ":" << port << std::endl;
            server.listen(listen.c_str(), port);
            std::cout << "Server has exited" << std::endl;
        } else {
            std::cout << "Unhandled command \"" << command << "\"" << std::endl;
            return 1;
        }
    }

    return 0;
}
