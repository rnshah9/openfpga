/*********************************************************************
 * Member functions for class Shell
 ********************************************************************/
#include <fstream>

/* Headers from vtrutil library */
#include "vtr_log.h"
#include "vtr_assert.h"

/* Headers from openfpgautil library */
#include "openfpga_tokenizer.h"

/* Headers from readline library */
#include <readline/readline.h>
#include <readline/history.h>

/* Headers from openfpgashell library */
#include "command_parser.h"
#include "command_echo.h"
#include "shell.h"

/* Begin namespace minishell */
namespace minishell {

/*********************************************************************
 * Public constructors
 ********************************************************************/
template<class T>
Shell<T>::Shell(const char* name) {
  name_ = std::string(name);
}

/************************************************************************
 * Public Accessors : aggregates
 ***********************************************************************/
template<class T>
std::string Shell<T>::name() const {
  return name_;
}

template<class T>
std::string Shell<T>::title() const {
  return title_;
}

template<class T>
typename Shell<T>::shell_command_range Shell<T>::commands() const {
  return vtr::make_range(command_ids_.begin(), command_ids_.end());
}

template<class T>
ShellCommandId Shell<T>::command(const std::string& name) const {
  /* Ensure that the name is unique in the command list */
  std::map<std::string, ShellCommandId>::const_iterator name_it = command_name2ids_.find(name);
  if (name_it == command_name2ids_.end()) {
    return ShellCommandId::INVALID();
  }
  return command_name2ids_.at(name);
}

template<class T>
std::string Shell<T>::command_description(const ShellCommandId& cmd_id) const {
  VTR_ASSERT(true == valid_command_id(cmd_id));
  return command_description_[cmd_id];
}

template<class T>
const Command& Shell<T>::command(const ShellCommandId& cmd_id) const {
  VTR_ASSERT(true == valid_command_id(cmd_id));
  return commands_[cmd_id];
}

template<class T>
const CommandContext& Shell<T>::command_context(const ShellCommandId& cmd_id) const {
  VTR_ASSERT(true == valid_command_id(cmd_id));
  return command_contexts_[cmd_id];
}

template<class T>
std::vector<ShellCommandId> Shell<T>::command_dependency(const ShellCommandId& cmd_id) const {
  VTR_ASSERT(true == valid_command_id(cmd_id));
  return command_dependencies_[cmd_id];
}

/************************************************************************
 * Public mutators
 ***********************************************************************/
template<class T>
void Shell<T>::set_title(const char* title) {
  title_ = std::string(title);
}

/* Add a command with it description */
template<class T>
ShellCommandId Shell<T>::add_command(const Command& cmd, const char* descr) {
  /* Ensure that the name is unique in the command list */
  std::map<std::string, ShellCommandId>::const_iterator name_it = command_name2ids_.find(std::string(cmd.name()));
  if (name_it != command_name2ids_.end()) {
    return ShellCommandId::INVALID();
  }

  /* This is a legal name. we can create a new id */
  ShellCommandId shell_cmd = ShellCommandId(command_ids_.size());
  command_ids_.push_back(shell_cmd);
  commands_.emplace_back(cmd);
  command_contexts_.push_back(CommandContext(cmd));
  command_description_.push_back(descr);
  command_execute_functions_.emplace_back();
  command_dependencies_.emplace_back();

  /* Register the name in the name2id map */
  command_name2ids_[std::string(name)] = cmd.name();

  return shell_cmd;
} 

template<class T>
void Shell<T>::add_command_execute_function(const ShellCommandId& cmd_id, 
                                            std::function<void(T, const CommandContext&)> exec_func) {
  VTR_ASSERT(true == valid_command_id(cmd_id));
  command_execute_functions_[cmd_id] = exec_func;
}

template<class T>
void Shell<T>::add_command_dependency(const ShellCommandId& cmd_id,
                                      const std::vector<ShellCommandId> dependent_cmds) {
  /* Validate the command id as well as each of the command dependency */
  VTR_ASSERT(true == valid_command_id(cmd_id));
  for (ShellCommandId dependent_cmd : dependent_cmds) {
    VTR_ASSERT(true == valid_command_id(dependent_cmd));
  }
  command_dependencies_[cmd_id] = dependent_cmds;
}

/************************************************************************
 * Public executors
 ***********************************************************************/
template <class T>
void Shell<T>::run_interactive_mode(T& context) {
  VTR_LOG("Start interactive mode of %s...\n",
          name().c_str());

  /* Print the title of the shell */
  if (!title().empty()) {
    VTR_LOG("%s\n", title());
  }

  /* Wait for users input and execute the command */
  char* cmd_line;
  while ((cmd_line = readline(std::string(name() + std::string("> ")).c_str())) != nullptr) {
    /* If the line is not empty:
     * Try to execute the command and
     * Add to history 
     */
    if (strlen(cmd_line) > 0) {
      execute_command((const char*)cmd_line, context);
      add_history(cmd_line);
    }

    /* Free the line as readline malloc a new line each time */
    free(cmd_line);
  }
}

template <class T>
void Shell<T>::run_script_mode(const char* script_file_name, T& context) {

  VTR_LOG("Reading script file %s...\n", script_file_name);

  /* Print the title of the shell */
  if (!title().empty()) {
    VTR_LOG("%s\n", title());
  } 

  std::string line;

  /* Create an input file stream */
  std::ifstream fp(script_file_name);

  if (!fp.is_open()) {
    /* Fail to open the file, ask user to check */
    VTR_LOG("Fail to open the script file: %s! Please check its location\n",
            script_file_name);
    return; 
  }

  /* Read line by line */
  while (getline(fp, line)) {
    /* If the line that starts with '#', it is commented, we can skip */ 
    if ('#' == line.front()) {
      continue;
    }
    /* Try to split the line with '#', the string before '#' is the read command we want */
    std::string cmd_part = line;
    std::size_t cmd_end_pos = line.find_first_of('#');
    /* If the full line has '#', we need the part before it */
    if (cmd_end_pos != std::string::npos) {
      cmd_part = line.substr(0, cmd_end_pos);
    }
    /* Process the command */
    execute_command(cmd_part.c_str(), context);
  }
  fp.close();
}

/************************************************************************
 * Private executors
 ***********************************************************************/
template <class T>
void Shell<T>::execute_command(const char* cmd_line,
                               T& common_context) {
  /* Tokenize the line */
  openfpga::StringToken tokenizer(cmd_line);  
  std::vector<std::string> tokens = tokenizer.split(" ");

  /* Find if the command name is valid */
  ShellCommandId cmd_id = command(tokens[0]);
  if (ShellCommandId::INVALID() == cmd_id) {
    VTR_LOG("Try to call a command '%s' which is not defined!\n",
            tokens[0].c_str());
    return;
  }

  /* Find the command! Parse the options */
  if (false == parse_command(tokens, commands_[cmd_id], command_contexts_[cmd_id])) {
    /* Echo the command */
    print_command_options(commands_[cmd_id]);
    return;
  }
    
  /* Parse succeed. Let user to confirm selected options */ 
  print_command_context(commands_[cmd_id], command_contexts_[cmd_id]);
  
  /* Execute the command! */ 
  command_execute_functions_[cmd_id](common_context, command_contexts_[cmd_id]);
}

/************************************************************************
 * Public invalidators/validators 
 ***********************************************************************/
template<class T>
bool Shell<T>::valid_command_id(const ShellCommandId& cmd_id) const {
  return ( size_t(cmd_id) < command_ids_.size() ) && ( cmd_id == command_ids_[cmd_id] ); 
}

} /* End namespace minshell */
