#include "chain.hpp"
#include <memory>

#include <fc/bitutil.hpp>
#include <fc/filesystem.hpp>
#include <fc/io/json.hpp>
#include <fc/variant.hpp>

#include <boost/exception/diagnostic_information.hpp>

#include <eosio/chain/block_log.hpp>
#include <eosio/chain/exceptions.hpp>
#include <eosio/chain/chainbase_environment.hpp>
#include <eosio/chain/controller.hpp>

#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>
#include <filesystem>

using namespace eosio;
using namespace eosio::chain;

void chain_actions::setup(CLI::App& app) {
   auto* sub = app.add_subcommand("chain-state", "chain utility");
   sub->add_option("--state-dir", opt->sstate_state_dir, "The location of the state directory (absolute path or relative to the current directory)")->capture_default_str();
   sub->add_option("--blocks-dir", opt->blocks_dir, "The location of the blocks directory (absolute path or relative to the current directory)")->capture_default_str();
   sub->require_subcommand();
   sub->fallthrough();

   auto* build = sub->add_subcommand("build-info", "extract build environment information as JSON");
   build->add_option("--output-file,-o", opt->build_output_file, "write into specified file")->capture_default_str();
   build->add_flag("--print,-p", opt->build_just_print, "print to console");
   build->require_option(1);

   build->callback([&]() {
      int rc = run_subcommand_build();
      // properly return err code in main
      if(rc) throw(CLI::RuntimeError(rc));
   });

  sub->add_subcommand("last-shutdown-state", "indicate whether last shutdown was clean or not")->callback([&]() {
      int rc = run_subcommand_sstate();
      // properly return err code in main
      if(rc) throw(CLI::RuntimeError(rc));
   });

   // -- replace-producer-keys
   build = sub->add_subcommand("replace-producer-keys", "Replace the producer keys and change chain id");
   build->add_option("--db-size", opt->db_size_mb, "Maximum size (in MiB) of the chain state database")->capture_default_str();
   auto* sub_opt = build->add_option("--key", opt->producer_key, "Public key to assign to all producers")->capture_default_str();
   sub_opt->required();
   build->callback([&]() {
      int rc = run_subcommand_replace_producer_keys();
      // properly return err code in main
      if (rc) throw(CLI::RuntimeError(rc));
   });

}

int chain_actions::run_subcommand_build() {
   if(!opt->build_output_file.empty()) {
      std::filesystem::path p = opt->build_output_file;
      if(p.is_relative()) {
         p = std::filesystem::current_path() / p;
      }
      fc::json::save_to_file(chainbase::environment(), p, true);
      std::cout << "Saved build info JSON to '" <<  p.generic_string() << "'" << std::endl;
   }
   if(opt->build_just_print) {
      std::cout << fc::json::to_pretty_string(chainbase::environment()) << std::endl;
   }

   return 0;
}

template <typename SubcommandOptions>
inline std::filesystem::path get_state_dir(std::shared_ptr<SubcommandOptions> opt) {
   std::filesystem::path state_dir;

   // default state dir, if none specified
   if(opt->sstate_state_dir.empty()) {
      auto root = fc::app_path();
      auto default_data_dir = root / "eosio" / "nodeos" / "data" ;
      state_dir  = default_data_dir / config::default_state_dir_name;
   } else {
      // adjust if path relative
      state_dir = opt->sstate_state_dir;
      if(state_dir.is_relative()) {
         state_dir = std::filesystem::current_path() / state_dir;
      }
   }

   return state_dir;
}

int chain_actions::run_subcommand_sstate() {
   std::filesystem::path state_dir = get_state_dir(opt);

   auto shared_mem_path = state_dir / "shared_memory.bin";

   if(!std::filesystem::exists(shared_mem_path)) {
      std::cerr << "Unable to read database status: file not found: " << shared_mem_path << std::endl;
      return -1;
   }

   char header[chainbase::header_size];
   std::ifstream hs(shared_mem_path.generic_string(), std::ifstream::binary);
   hs.read(header, chainbase::header_size);
   if(hs.fail()) {
      std::cerr << "Unable to read database status: file invalid or corrupt" << shared_mem_path <<  std::endl;
      return -1;
   }

   chainbase::db_header* dbheader = reinterpret_cast<chainbase::db_header*>(header);
   if(dbheader->id != chainbase::header_id) {
      std::string what_str("\"" + state_dir.generic_string() + "\" database format not compatible with this version of chainbase.");
      std::cerr << what_str << std::endl;
      return -1;
   }
   if(dbheader->dirty) {
      std::cout << "Database dirty flag is set, shutdown was not clean" << std::endl;
      return -1;
   }

   std::cout << "Database state is clean" << std::endl;
   return 0;
}

int chain_actions::run_subcommand_replace_producer_keys() {
   std::filesystem::path state_dir = get_state_dir(opt);

   auto shared_mem_path = state_dir / "shared_memory.bin";

   if(!std::filesystem::exists(shared_mem_path)) {
      std::cerr << "Unable to read database status: file not found: " << shared_mem_path << std::endl;
      return -1;
   }

   if (opt->blocks_dir.empty()) {
      std::cerr << "--blocks-dir required " << std::endl;
      return -1;
   }

   public_key_type producer_key(opt->producer_key);

   // setup controller
   fc::temp_directory dir;
   const auto& temp_dir = dir.path();
   std::filesystem::path finalizers_dir = temp_dir / "finalizers";
   controller::config cfg;
   cfg.blocks_dir = opt->blocks_dir;
   cfg.finalizers_dir = finalizers_dir;
   cfg.state_dir  = state_dir;
   cfg.state_size = opt->db_size_mb * 1024 * 1024;
   cfg.eosvmoc_tierup = wasm_interface::vm_oc_enable::oc_none; // wasm not used, no use to fire up oc
   protocol_feature_set pfs = initialize_protocol_features( std::filesystem::path("protocol_features"), false );

   try {
      auto chain_id = controller::extract_chain_id_from_db( state_dir );
      if (!chain_id) {
         std::cerr << "Unable to extract chain id from state: " << state_dir << std::endl;
         return -1;
      }

      auto check_shutdown = []() { return false; };
      auto shutdown = []() { throw; };

      controller control(cfg, std::move(pfs), *chain_id);
      control.add_indices();
      control.startup(shutdown, check_shutdown);

      if (!opt->producer_key.empty()) {
         control.replace_producer_keys(producer_key);
         control.replace_account_keys(config::system_account_name, config::active_name, producer_key);
      }
   } catch(const database_guard_exception& e) {
      std::cerr << "Database is not configured to have enough storage to handle provided snapshot, please increase storage and try again" << std::endl;
      return -1;
   } catch (const fc::exception& ex) {
      std::cerr << "Exception: " << ex.to_detail_string() << std::endl;
      return -1;
   } catch (const std::exception& ex) {
      std::cerr << "STD Exception: " << ex.what() << std::endl;
      return -1;
   }

   return 0;
}