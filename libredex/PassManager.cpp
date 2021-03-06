/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "PassManager.h"

#include <cstdio>
#include <unordered_set>

#include "ConfigFiles.h"
#include "Debug.h"
#include "DexClass.h"
#include "DexLoader.h"
#include "DexOutput.h"
#include "DexUtil.h"
#include "InterDex.h"
#include "IRCode.h"
#include "IRTypeChecker.h"
#include "ParallelWalkers.h"
#include "PrintSeeds.h"
#include "ProguardMatcher.h"
#include "ProguardPrintConfiguration.h"
#include "ProguardReporting.h"
#include "ReachableClasses.h"
#include "Timer.h"

redex::ProguardConfiguration empty_pg_config() {
  redex::ProguardConfiguration pg_config;
  return pg_config;
}

PassManager::PassManager(const std::vector<Pass*>& passes,
                         const Json::Value& config,
                         bool verify_none_mode)
    : m_config(config),
      m_registered_passes(passes),
      m_current_pass_info(nullptr),
      m_pg_config(empty_pg_config()),
      m_testing_mode(false),
      m_verify_none_mode(verify_none_mode) {
  init(config);
}

PassManager::PassManager(const std::vector<Pass*>& passes,
                         const redex::ProguardConfiguration& pg_config,
                         const Json::Value& config,
                         bool verify_none_mode)
    : m_config(config),
      m_registered_passes(passes),
      m_current_pass_info(nullptr),
      m_pg_config(pg_config),
      m_testing_mode(false),
      m_verify_none_mode(verify_none_mode) {
  init(config);
}

void PassManager::init(const Json::Value& config) {
  if (config["redex"].isMember("passes")) {
    auto passes_from_config = config["redex"]["passes"];
    for (auto& pass : passes_from_config) {
      activate_pass(pass.asString().c_str(), config);
    }
  } else {
    // If config isn't set up, run all registered passes.
    m_activated_passes = m_registered_passes;
  }
}

void PassManager::run_type_checker(const Scope& scope, bool verify_moves) {
  TRACE(PM, 1, "Running IRTypeChecker...\n");
  Timer t("IRTypeChecker");
  walk_methods_parallel_simple(scope, [verify_moves](DexMethod* dex_method) {
    IRTypeChecker checker(dex_method, verify_moves);
    if (checker.fail()) {
      std::string msg = checker.what();
      fprintf(
          stderr, "ABORT! Inconsistency found in Dex code. %s", msg.c_str());
      exit(EXIT_FAILURE);
    }
  });
}

const std::string PASS_ORDER_KEY = "pass_order";

void PassManager::run_passes(DexStoresVector& stores,
                             const Scope& external_classes,
                             ConfigFiles& cfg) {
  DexStoreClassesIterator it(stores);
  Scope scope = build_class_scope(it);
  {
    Timer t("Initializing reachable classes");
    init_reachable_classes(
        scope, m_config, m_pg_config, cfg.get_no_optimizations_annos());
  }
  {
    Timer t("Processing proguard rules");
    process_proguard_rules(
        cfg.get_proguard_map(), scope, external_classes, &m_pg_config);
  }
  char* seeds_output_file = std::getenv("REDEX_SEEDS_FILE");
  if (seeds_output_file) {
    std::string seed_filename = seeds_output_file;
    Timer t("Writing seeds file " + seed_filename);
    std::ofstream seeds_file(seed_filename);
    redex::print_seeds(seeds_file, cfg.get_proguard_map(), scope, false, false);
  }
  if (!cfg.get_printseeds().empty()) {
    Timer t("Writing seeds to file " + cfg.get_printseeds());
    std::ofstream seeds_file(cfg.get_printseeds());
    redex::print_seeds(seeds_file, cfg.get_proguard_map(), scope);
    std::ofstream config_file(cfg.get_printseeds() + ".pro");
    redex::show_configuration(config_file, scope, m_pg_config);
    std::ofstream incoming(cfg.get_printseeds() + ".incoming");
    redex::print_classes(incoming, cfg.get_proguard_map(), scope);
    std::ofstream shrinking_file(cfg.get_printseeds() + ".allowshrinking");
    redex::print_seeds(
        shrinking_file, cfg.get_proguard_map(), scope, true, false);
    std::ofstream obfuscation_file(cfg.get_printseeds() + ".allowobfuscation");
    redex::print_seeds(
        obfuscation_file, cfg.get_proguard_map(), scope, false, true);
  }

  // Count the number of appearances of each pass name.
  const auto pass_repeats = [&]() {
    std::unordered_map<const Pass*, size_t> pass_repeats;
    for (const auto& pass : m_activated_passes) {
      ++pass_repeats[pass];
    }
    return pass_repeats;
  }();

  std::unordered_map<const Pass*, size_t> pass_counters;
  m_pass_info.resize(m_activated_passes.size());
  for (size_t i = 0; i < m_activated_passes.size(); ++i) {
    Pass* pass = m_activated_passes[i];
    TRACE(PM, 1, "Evaluating %s...\n", pass->name().c_str());
    Timer t(pass->name() + " (eval)");
    const size_t count = pass_counters[pass]++;
    m_pass_info[i].pass = pass;
    m_pass_info[i].order = i;
    m_pass_info[i].repeat = count;
    m_pass_info[i].total_repeat = pass_repeats.at(pass);
    m_pass_info[i].name = pass->name() + "#" + std::to_string(count + 1);
    m_pass_info[i].metrics[PASS_ORDER_KEY] = i;
    m_current_pass_info = &m_pass_info[i];
    pass->eval_pass(stores, cfg, *this);
    m_current_pass_info = nullptr;
  }

  // Retrieve the type checker's settings.
  bool type_checker_enabled = m_config.isMember("ir_type_checker");
  auto type_checker_args = m_config["ir_type_checker"];
  bool run_after_each_pass =
      type_checker_args.get("run_after_each_pass", false).asBool();
  bool verify_moves = type_checker_args.get("verify_moves", false).asBool();
  std::unordered_set<std::string> trigger_passes;

  for (auto& trigger_pass : type_checker_args["run_after_passes"]) {
    trigger_passes.insert(trigger_pass.asString());
  }

  for (size_t i = 0; i < m_activated_passes.size(); ++i) {
    Pass* pass = m_activated_passes[i];
    TRACE(PM, 1, "Running %s...\n", pass->name().c_str());
    Timer t(pass->name() + " (run)");
    m_current_pass_info = &m_pass_info[i];
    pass->run_pass(stores, cfg, *this);
    if (type_checker_enabled &&
        (run_after_each_pass || trigger_passes.count(pass->name()) > 0)) {
      scope = build_class_scope(it);
      run_type_checker(scope, verify_moves);
    }
    m_current_pass_info = nullptr;
  }

  // Always run the type checker before generating the optimized dex code.
  if (type_checker_enabled) {
    scope = build_class_scope(it);
    run_type_checker(scope, verify_moves);
  }

  if (!cfg.get_printseeds().empty()) {
    Timer t("Writing outgoing classes to file " + cfg.get_printseeds() +
            ".outgoing");
    // Recompute the scope.
    scope = build_class_scope(it);
    std::ofstream outgoig(cfg.get_printseeds() + ".outgoing");
    redex::print_classes(outgoig, cfg.get_proguard_map(), scope);
    redex::alert_seeds(std::cerr, scope);
  }
}

void PassManager::activate_pass(const char* name, const Json::Value& cfg) {
  for (auto pass : m_registered_passes) {
    if (name == pass->name()) {
      m_activated_passes.push_back(pass);
      pass->configure_pass(PassConfig(cfg[pass->name()]));
      return;
    }
  }
  always_assert_log(false, "No pass named %s!", name);
}

void PassManager::incr_metric(const std::string& key, int value) {
  always_assert_log(m_current_pass_info != nullptr, "No current pass!");
  (m_current_pass_info->metrics)[key] += value;
}

void PassManager::set_metric(const std::string& key, int value) {
  always_assert_log(m_current_pass_info != nullptr, "No current pass!");
  (m_current_pass_info->metrics)[key] = value;
}

int PassManager::get_metric(const std::string& key) {
  return (m_current_pass_info->metrics)[key];
}

const std::vector<PassManager::PassInfo>& PassManager::get_pass_info() const {
  return m_pass_info;
}

const std::unordered_map<std::string, int>&
PassManager::get_interdex_metrics() {
  for (const auto& pass_info : m_pass_info) {
    if (pass_info.pass->name() == INTERDEX_PASS_NAME) {
      return pass_info.metrics;
    }
  }
  static std::unordered_map<std::string, int> empty;
  return empty;
}
