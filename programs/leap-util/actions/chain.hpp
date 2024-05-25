#include "subcommand.hpp"

struct chain_options {
   bool build_just_print = false;
   std::string build_output_file;
   std::string sstate_state_dir;
   std::string blocks_dir;
   std::string producer_key;
   uint64_t db_size = 65536ull;
};

class chain_actions : public sub_command<chain_options> {
public:
   chain_actions() : sub_command() {}
   void setup(CLI::App& app);

   // callbacks
   int run_subcommand_build();
   int run_subcommand_sstate();
   int run_subcommand_replace_producer_keys();
};