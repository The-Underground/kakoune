#include "commands.hh"

#include "buffer.hh"
#include "buffer_manager.hh"
#include "client_manager.hh"
#include "color_registry.hh"
#include "command_manager.hh"
#include "completion.hh"
#include "context.hh"
#include "debug.hh"
#include "event_manager.hh"
#include "file.hh"
#include "highlighter.hh"
#include "highlighters.hh"
#include "client.hh"
#include "option_manager.hh"
#include "option_types.hh"
#include "parameters_parser.hh"
#include "register_manager.hh"
#include "shell_manager.hh"
#include "string.hh"
#include "user_interface.hh"
#include "utf8_iterator.hh"
#include "window.hh"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

namespace Kakoune
{

namespace
{

Buffer* open_or_create(const String& filename, Context& context)
{
    Buffer* buffer = create_buffer_from_file(filename);
    if (not buffer)
    {
        context.print_status({ "new file " + filename, get_color("StatusLine") });
        buffer = new Buffer(filename, Buffer::Flags::File | Buffer::Flags::New);
    }
    return buffer;
}

Buffer* open_fifo(const String& name , const String& filename, Context& context)
{
    int fd = open(parse_filename(filename).c_str(), O_RDONLY);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (fd < 0)
       throw runtime_error("unable to open " + filename);

    BufferManager::instance().delete_buffer_if_exists(name);

    Buffer* buffer = new Buffer(name, Buffer::Flags::Fifo | Buffer::Flags::NoUndo);

    auto watcher = new FDWatcher(fd, [buffer](FDWatcher& watcher) {
        constexpr size_t buffer_size = 1024 * 16;
        char data[buffer_size];
        ssize_t count = read(watcher.fd(), data, buffer_size);
        buffer->insert(buffer->end()-1, count > 0 ? String(data, data+count)
                                                  : "*** kak: fifo closed ***\n");
        if (count <= 0)
        {
            kak_assert(buffer->flags() & Buffer::Flags::Fifo);
            buffer->flags() &= ~Buffer::Flags::Fifo;
            buffer->flags() &= ~Buffer::Flags::NoUndo;
            close(watcher.fd());
            delete &watcher;
        }
    });

    buffer->hooks().add_hook("BufClose", "",
        [buffer, watcher](const String&, const Context&) {
            // Check if fifo is still alive, else watcher is already dead
            if (buffer->flags() & Buffer::Flags::Fifo)
            {
                close(watcher->fd());
                delete watcher;
            }
        });

    return buffer;
}

template<bool force_reload>
void edit(CommandParameters params, Context& context)
{
    ParametersParser parser(params, { { "scratch", false },
                                      { "fifo", true } },
                            ParametersParser::Flags::None, 1, 3);

    const String name = parser[0];

    Buffer* buffer = nullptr;
    if (not force_reload)
        buffer = BufferManager::instance().get_buffer_ifp(name);
    if (not buffer)
    {
        if (parser.has_option("scratch"))
        {
            BufferManager::instance().delete_buffer_if_exists(name);
            buffer = new Buffer(name, Buffer::Flags::None);
        }
        else if (parser.has_option("fifo"))
            buffer = open_fifo(name, parser.option_value("fifo"), context);
        else
            buffer = open_or_create(name, context);
    }

    BufferManager::instance().set_last_used_buffer(*buffer);

    const size_t param_count = parser.positional_count();
    if (buffer != &context.buffer() or param_count > 1)
        context.push_jump();

    if (buffer != &context.buffer())
        context.change_buffer(*buffer);

    if (param_count > 1 and not parser[1].empty())
    {
        int line = std::max(0, str_to_int(parser[1]) - 1);
        int column = param_count > 2 and not parser[2].empty() ?
                     std::max(0, str_to_int(parser[2]) - 1) : 0;

        context.selections() = context.buffer().clamp({ line,  column });
        if (context.has_window())
            context.window().center_line(context.selections().main().last().line);
    }
}

void write_buffer(CommandParameters params, Context& context)
{
    if (params.size() > 1)
        throw wrong_argument_count();

    Buffer& buffer = context.buffer();

    if (params.empty() and !(buffer.flags() & Buffer::Flags::File))
        throw runtime_error("cannot write a non file buffer without a filename");

    String filename = params.empty() ? buffer.name()
                                     : parse_filename(params[0]);

    write_buffer_to_file(buffer, filename);
}

void write_all_buffers(CommandParameters params, Context& context)
{
    if (params.size() != 0)
        throw wrong_argument_count();

    for (auto& buffer : BufferManager::instance())
    {
        if ((buffer->flags() & Buffer::Flags::File) and buffer->is_modified())
            write_buffer_to_file(*buffer, buffer->name());
    }
}

template<bool force>
void quit(CommandParameters params, Context& context)
{
    if (params.size() != 0)
        throw wrong_argument_count();

    if (not force and ClientManager::instance().count() == 1)
    {
        std::vector<String> names;
        for (auto& buffer : BufferManager::instance())
        {
            if ((buffer->flags() & Buffer::Flags::File) and buffer->is_modified())
                names.push_back(buffer->name());
        }
        if (not names.empty())
        {
            String message = "modified buffers remaining: [";
            for (auto it = names.begin(); it != names.end(); ++it)
            {
                if (it != names.begin())
                    message += ", ";
                message += *it;
            }
            message += "]";
            throw runtime_error(message);
        }
    }
    // unwind back to this client event handler.
    throw client_removed{};
}

template<bool force>
void write_and_quit(CommandParameters params, Context& context)
{
    write_buffer(params, context);
    quit<force>(CommandParameters(), context);
}

void show_buffer(CommandParameters params, Context& context)
{
    if (params.size() != 1)
        throw wrong_argument_count();

    Buffer& buffer = BufferManager::instance().get_buffer(params[0]);
    BufferManager::instance().set_last_used_buffer(buffer);

    if (&buffer != &context.buffer())
    {
        context.push_jump();
        context.change_buffer(buffer);
    }
}

template<bool force>
void delete_buffer(CommandParameters params, Context& context)
{
    if (params.size() > 1)
        throw wrong_argument_count();

    BufferManager& manager = BufferManager::instance();
    Buffer& buffer = params.empty() ? context.buffer() : manager.get_buffer(params[0]);
    if (not force and (buffer.flags() & Buffer::Flags::File) and buffer.is_modified())
        throw runtime_error("buffer " + buffer.name() + " is modified");

    if (manager.count() == 1)
        throw runtime_error("buffer " + buffer.name() + " is the last one");

    manager.delete_buffer(buffer);
}

void set_buffer_name(CommandParameters params, Context& context)
{
    ParametersParser parser(params, OptionMap{},
                            ParametersParser::Flags::None, 1, 1);
    if (not context.buffer().set_name(parser[0]))
        throw runtime_error("unable to change buffer name to " + parser[0]);
}

void define_highlighter(CommandParameters params, Context& context)
{
    if (params.size() != 1)
        throw wrong_argument_count();

    const String& name = params[0];
    DefinedHighlighters::instance().append({name, HighlighterGroup{}});
}

void add_highlighter(CommandParameters params, Context& context)
{
    ParametersParser parser(params, { { "group", true }, { "def-group", true } }, ParametersParser::Flags::None, 1);
    HighlighterRegistry& registry = HighlighterRegistry::instance();

    auto begin = parser.begin();
    const String& name = *begin;
    std::vector<String> highlighter_params;
    for (++begin; begin != parser.end(); ++begin)
        highlighter_params.push_back(*begin);

    if (parser.has_option("group") and parser.has_option("def-group"))
        throw runtime_error("-group and -def-group cannot be specified together");

    HighlighterGroup* group = nullptr;

    if (parser.has_option("def-group"))
        group = &DefinedHighlighters::instance().get_group(parser.option_value("def-group"), '/');
    else
    {
        HighlighterGroup& window_hl = context.window().highlighters();
        group = parser.has_option("group") ?
                &window_hl.get_group(parser.option_value("group"), '/')
              : &window_hl;
    }

    group->append(registry[name](highlighter_params));
}

void rm_highlighter(CommandParameters params, Context& context)
{
    ParametersParser parser(params, { { "group", true } }, ParametersParser::Flags::None, 1, 1);

    HighlighterGroup& window_hl = context.window().highlighters();
    HighlighterGroup& group = parser.has_option("group") ?
        window_hl.get_group(parser.option_value("group"), '/')
      : window_hl;

    group.remove(parser[0]);
}

static HookManager& get_hook_manager(const String& scope, Context& context)
{
    if (prefix_match("global", scope))
        return GlobalHooks::instance();
    else if (prefix_match("buffer", scope))
        return context.buffer().hooks();
    else if (prefix_match("window", scope))
        return context.window().hooks();
    throw runtime_error("error: no such hook container " + scope);
}

void add_hook(CommandParameters params, Context& context)
{
    ParametersParser parser(params, { { "id", true } }, ParametersParser::Flags::None, 4, 4);
    // copy so that the lambda gets a copy as well
    Regex regex(parser[2].begin(), parser[2].end());
    String command = parser[3];
    auto hook_func = [=](const String& param, Context& context) {
        if (boost::regex_match(param.begin(), param.end(), regex))
            CommandManager::instance().execute(command, context, {},
                                               { { "hook_param", param } });
    };
    String id = parser.has_option("id") ? parser.option_value("id") : "";
    get_hook_manager(parser[0], context).add_hook(parser[1], id, hook_func);
}

void rm_hooks(CommandParameters params, Context& context)
{
    ParametersParser parser(params, {}, ParametersParser::Flags::None, 2, 2);
    get_hook_manager(parser[0], context).remove_hooks(parser[1]);
}

EnvVarMap params_to_env_var_map(CommandParameters params)
{
    std::unordered_map<String, String> vars;
    char param_name[] = "param0";
    for (size_t i = 0; i < params.size(); ++i)
    {
        param_name[sizeof(param_name) - 2] = '0' + i;
        vars[param_name] = params[i];
    }
    return vars;
}

void define_command(CommandParameters params, Context& context)
{
    ParametersParser parser(params,
                            { { "env-params", false },
                              { "shell-params", false },
                              { "allow-override", false },
                              { "file-completion", false },
                              { "hidden", false },
                              { "shell-completion", true } },
                             ParametersParser::Flags::None,
                             2, 2);

    auto begin = parser.begin();
    const String& cmd_name = *begin;

    if (CommandManager::instance().command_defined(cmd_name) and
        not parser.has_option("allow-override"))
        throw runtime_error("command '" + cmd_name + "' already defined");

    CommandFlags flags = CommandFlags::None;
    if (parser.has_option("hidden"))
        flags = CommandFlags::Hidden;

    String commands = parser[1];
    Command cmd;
    if (parser.has_option("env-params"))
    {
        cmd = [=](CommandParameters params, Context& context) {
            CommandManager::instance().execute(commands, context, {},
                                               params_to_env_var_map(params));
        };
    }
    if (parser.has_option("shell-params"))
    {
        cmd = [=](CommandParameters params, Context& context) {
            CommandManager::instance().execute(commands, context, params);
        };
    }
    else
    {
        cmd = [=](CommandParameters params, Context& context) {
            if (not params.empty())
                throw wrong_argument_count();
            CommandManager::instance().execute(commands, context);
        };
    }

    CommandCompleter completer;
    if (parser.has_option("file-completion"))
    {
        completer = [](const Context& context, CompletionFlags flags,
                       CommandParameters params,
                       size_t token_to_complete, ByteCount pos_in_token)
        {
             const String& prefix = token_to_complete < params.size() ?
                                    params[token_to_complete] : String();
             return complete_filename(prefix, context.options()["ignored_files"].get<Regex>(), pos_in_token);
        };
    }
    else if (parser.has_option("shell-completion"))
    {
        String shell_cmd = parser.option_value("shell-completion");
        completer = [=](const Context& context, CompletionFlags flags,
                        CommandParameters params,
                        size_t token_to_complete, ByteCount pos_in_token)
        {
            if (flags == CompletionFlags::Fast) // no shell on fast completion
                return CandidateList{};
            EnvVarMap vars = {
                { "token_to_complete", to_string(token_to_complete) },
                { "pos_in_token",      to_string(pos_in_token) }
            };
            String output = ShellManager::instance().eval(shell_cmd, context, params, vars);
            return split(output, '\n');
        };
    }
    CommandManager::instance().register_command(cmd_name, cmd, flags, completer);
}

void echo_message(CommandParameters params, Context& context)
{
    ParametersParser parser(params, { { "color", true } },
                            ParametersParser::Flags::OptionsOnlyAtStart);
    String message;
    for (auto& param : parser)
        message += param + " ";
    ColorPair color = get_color(parser.has_option("color") ?
                                parser.option_value("color") : "StatusLine");
    context.print_status({ std::move(message), color } );
}

void write_debug_message(CommandParameters params, Context&)
{
    String message;
    for (auto& param : params)
        message += param + " ";
    write_debug(message);
}

void exec_commands_in_file(CommandParameters params,
                           Context& context)
{
    if (params.size() != 1)
        throw wrong_argument_count();

    String file_content = read_file(parse_filename(params[0]));
    try
    {
        CommandManager::instance().execute(file_content, context);
    }
    catch (Kakoune::runtime_error& err)
    {
        write_debug("error while executing commands in file '" + params[0]
                    + "'\n    " + err.what());
        throw;
    }
}

static OptionManager& get_options(const String& scope, const Context& context)
{
    if (prefix_match("global", scope))
        return GlobalOptions::instance();
    else if (prefix_match("buffer", scope))
        return context.buffer().options();
    else if (prefix_match("window", scope))
        return context.window().options();
    else if (prefix_match(scope, "buffer="))
        return BufferManager::instance().get_buffer(scope.substr(7_byte)).options();
    throw runtime_error("error: no such option container " + scope);
}

void set_option(CommandParameters params, Context& context)
{
    ParametersParser parser(params, { { "add", false } },
                            ParametersParser::Flags::OptionsOnlyAtStart,
                            3, 3);

    Option& opt = get_options(parser[0], context).get_local_option(parser[1]);
    if (parser.has_option("add"))
        opt.add_from_string(parser[2]);
    else
        opt.set_from_string(parser[2]);
}

void declare_option(CommandParameters params, Context& context)
{
    ParametersParser parser(params, { { "hidden", false } },
                            ParametersParser::Flags::OptionsOnlyAtStart,
                            2, 3);
    Option* opt = nullptr;

    Option::Flags flags = Option::Flags::None;
    if (parser.has_option("hidden"))
        flags = Option::Flags::Hidden;

    GlobalOptions& opts = GlobalOptions::instance();

    if (parser[0] == "int")
        opt = &opts.declare_option<int>(parser[1], 0, flags);
    if (parser[0] == "bool")
        opt = &opts.declare_option<bool>(parser[1], 0, flags);
    else if (parser[0] == "str")
        opt = &opts.declare_option<String>(parser[1], "", flags);
    else if (parser[0] == "regex")
        opt = &opts.declare_option<Regex>(parser[1], Regex{}, flags);
    else if (parser[0] == "int-list")
        opt = &opts.declare_option<std::vector<int>>(parser[1], {}, flags);
    else if (parser[0] == "str-list")
        opt = &opts.declare_option<std::vector<String>>(parser[1], {}, flags);
    else if (parser[0] == "line-flag-list")
        opt = &opts.declare_option<std::vector<LineAndFlag>>(parser[1], {}, flags);
    else
        throw runtime_error("unknown type " + parser[0]);

    if (parser.positional_count() == 3)
        opt->set_from_string(parser[2]);
}


KeymapManager& get_keymap_manager(const String& scope, Context& context)
{
    if (prefix_match("global", scope))
        return GlobalKeymaps::instance();
    else if (prefix_match("buffer", scope))
        return context.buffer().keymaps();
    else if (prefix_match("window", scope))
        return context.window().keymaps();
    throw runtime_error("error: no such keymap container " + scope);
}

KeymapMode parse_keymap_mode(const String& str)
{
    if (prefix_match("normal", str)) return KeymapMode::Normal;
    if (prefix_match("insert", str)) return KeymapMode::Insert;
    if (prefix_match("menu", str))   return KeymapMode::Menu;
    if (prefix_match("prompt", str)) return KeymapMode::Prompt;
    throw runtime_error("unknown keymap mode '" + str + "'");
}

void map_key(CommandParameters params, Context& context)
{
    ParametersParser parser(params, {}, ParametersParser::Flags::None, 4, 4);

    KeymapManager& keymaps = get_keymap_manager(params[0], context);
    KeymapMode keymap_mode = parse_keymap_mode(params[1]);

    KeyList key = parse_keys(params[2]);
    if (key.size() != 1)
        throw runtime_error("only a single key can be mapped");

    KeyList mapping = parse_keys(params[3]);
    keymaps.map_key(key[0], keymap_mode, std::move(mapping));
}

template<typename Func>
void context_wrap(CommandParameters params, Context& context, Func func)
{
    ParametersParser parser(params, { { "client", true }, { "try-client", true },
                                      { "draft", false }, { "itersel", false } },
                            ParametersParser::Flags::OptionsOnlyAtStart, 1);

    ClientManager& cm = ClientManager::instance();
    Context* real_context = &context;
    if (parser.has_option("client"))
        real_context = &cm.get_client(parser.option_value("client")).context();
    else if (parser.has_option("try-client"))
    {
        Client* client = cm.get_client_ifp(parser.option_value("try-client"));
        if (client)
            real_context = &client->context();
    }

    if (parser.has_option("draft"))
    {
        InputHandler input_handler(real_context->buffer(), real_context->selections(), real_context->name());

        // We do not want this draft context to commit undo groups if the real one is
        // going to commit the whole thing later
        if (real_context->is_editing())
            input_handler.context().disable_undo_handling();

        if (parser.has_option("itersel"))
        {
            DynamicSelectionList sels{real_context->buffer(), real_context->selections()};
            for (auto& sel : sels)
            {
                input_handler.context().selections() = sel;
                func(parser, input_handler.context());
            }
        }
        else
            func(parser, input_handler.context());
    }
    else
    {
        if (parser.has_option("itersel"))
            throw runtime_error("-itersel makes no sense without -draft");
        func(parser, *real_context);
    }

    // force redraw of this client window
    if (real_context != &context and real_context->has_window())
        real_context->window().forget_timestamp();
}

void exec_string(CommandParameters params, Context& context)
{
    context_wrap(params, context, [](const ParametersParser& parser, Context& context) {
        KeyList keys;
        for (auto& param : parser)
        {
            KeyList param_keys = parse_keys(param);
            keys.insert(keys.end(), param_keys.begin(), param_keys.end());
        }
        exec_keys(keys, context);
    });
}

void eval_string(CommandParameters params, Context& context)
{
    context_wrap(params, context, [](const ParametersParser& parser, Context& context) {
        String command;
        for (auto& param : parser)
            command += param + " ";
        CommandManager::instance().execute(command, context);
    });
}

void menu(CommandParameters params, Context& context)
{
    ParametersParser parser(params, { { "auto-single", false },
                                      { "select-cmds", false } });

    const bool with_select_cmds = parser.has_option("select-cmds");
    const size_t modulo = with_select_cmds ? 3 : 2;

    const size_t count = parser.positional_count();
    if (count == 0 or (count % modulo) != 0)
        throw wrong_argument_count();

    if (count == modulo and parser.has_option("auto-single"))
    {
        CommandManager::instance().execute(parser[1], context);
        return;
    }

    std::vector<String> choices;
    std::vector<String> commands;
    std::vector<String> select_cmds;
    for (int i = 0; i < count; i += modulo)
    {
        choices.push_back(parser[i]);
        commands.push_back(parser[i+1]);
        if (with_select_cmds)
            select_cmds.push_back(parser[i+2]);
    }

    context.input_handler().menu(choices,
        [=](int choice, MenuEvent event, Context& context) {
            if (event == MenuEvent::Validate and choice >= 0 and choice < commands.size())
              CommandManager::instance().execute(commands[choice], context);
            if (event == MenuEvent::Select and choice >= 0 and choice < select_cmds.size())
              CommandManager::instance().execute(select_cmds[choice], context);
        });
}

void info(CommandParameters params, Context& context)
{
    ParametersParser parser(params, { { "anchor", true }, { "title", true } },
                            ParametersParser::Flags::None, 0, 1);

    context.ui().info_hide();
    if (parser.positional_count() > 0)
    {
        MenuStyle style = MenuStyle::Prompt;
        DisplayCoord pos = context.ui().dimensions();
        pos.column -= 1;
        if (parser.has_option("anchor"))
        {
            style =  MenuStyle::Inline;
            const auto& sel = context.selections().main();
            auto it = sel.last();
            String anchor = parser.option_value("anchor");
            if (anchor == "left")
                it = sel.min();
            else if (anchor == "right")
                it = sel.max();
            else if (anchor != "cursor")
                throw runtime_error("anchor param must be one of [left, right, cursor]");
            pos = context.window().display_position(it);
        }
        const String& title = parser.has_option("title") ? parser.option_value("title") : "";
        context.ui().info_show(title, parser[0], pos, get_color("Information"), style);
    }
}

void try_catch(CommandParameters params, Context& context)
{
    if (params.size() != 1 and params.size() != 3)
        throw wrong_argument_count();

    const bool do_catch = params.size() == 3;
    if (do_catch and params[1] != "catch")
        throw runtime_error("usage: try <commands> [catch <on error commands>]");

    CommandManager& command_manager = CommandManager::instance();
    try
    {
        command_manager.execute(params[0], context);
    }
    catch (Kakoune::runtime_error& e)
    {
        if (do_catch)
            command_manager.execute(params[2], context);
    }
}

void define_color_alias(CommandParameters params, Context& context)
{
    ParametersParser parser(params, OptionMap{},
                            ParametersParser::Flags::None, 2, 2);
    ColorRegistry::instance().register_alias(
        parser[0], parser[1], true);
}

void set_client_name(CommandParameters params, Context& context)
{
    ParametersParser parser(params, OptionMap{},
                            ParametersParser::Flags::None, 1, 1);
    if (ClientManager::instance().validate_client_name(params[0]))
        context.set_name(params[0]);
    else if (context.name() != params[0])
        throw runtime_error("client name '" + params[0] + "' is not unique");
}

void set_register(CommandParameters params, Context& context)
{
    if (params.size() != 2)
        throw wrong_argument_count();

    if (params[0].length() != 1)
        throw runtime_error("register names are single character");
    RegisterManager::instance()[params[0][0]] = memoryview<String>(params[1]);
}

void change_working_directory(CommandParameters params, Context&)
{
    if (params.size() != 1)
        throw wrong_argument_count();

    if (chdir(parse_filename(params[0]).c_str()) != 0)
        throw runtime_error("cannot change to directory " + params[0]);
}

template<typename GetRootGroup>
CommandCompleter group_rm_completer(GetRootGroup get_root_group)
{
    return [=](const Context& context, CompletionFlags flags,
               CommandParameters params, size_t token_to_complete,
               ByteCount pos_in_token) {
        auto& root_group = get_root_group(context);
        const String& arg = token_to_complete < params.size() ?
                            params[token_to_complete] : String();
        if (token_to_complete == 1 and params[0] == "-group")
            return root_group.complete_group_id(arg, pos_in_token);
        else if (token_to_complete == 2 and params[0] == "-group")
            return root_group.get_group(params[1], '/').complete_id(arg, pos_in_token);
        return root_group.complete_id(arg, pos_in_token);
    };
}

template<typename FactoryRegistry, typename GetRootGroup>
CommandCompleter group_add_completer(GetRootGroup get_root_group)
{
    return [=](const Context& context, CompletionFlags flags,
               CommandParameters params, size_t token_to_complete,
               ByteCount pos_in_token) {
        auto& root_group = get_root_group(context);
        const String& arg = token_to_complete < params.size() ?
                            params[token_to_complete] : String();
        if (token_to_complete == 1 and params[0] == "-group")
            return root_group.complete_group_id(arg, pos_in_token);
        else if (token_to_complete == 0 or (token_to_complete == 2 and params[0] == "-group"))
            return FactoryRegistry::instance().complete_name(arg, pos_in_token);
        return CandidateList();
    };
}

class RegisterRestorer
{
public:
    RegisterRestorer(char name, const Context& context)
      : m_name(name)
    {
        memoryview<String> save = RegisterManager::instance()[name].values(context);
        m_save = std::vector<String>(save.begin(), save.end());
    }

    ~RegisterRestorer()
    { RegisterManager::instance()[m_name] = m_save; }

private:
    std::vector<String> m_save;
    char                m_name;
};

}

void exec_keys(const KeyList& keys, Context& context)
{
    RegisterRestorer quote('"', context);
    RegisterRestorer slash('/', context);

    ScopedEdition edition(context);

    for (auto& key : keys)
        context.input_handler().handle_key(key);
}

void register_commands()
{
    CommandManager& cm = CommandManager::instance();

    cm.register_commands({"nop"}, [](CommandParameters, Context&){});

    PerArgumentCommandCompleter filename_completer({
         [](const Context& context, CompletionFlags flags, const String& prefix, ByteCount cursor_pos)
         { return complete_filename(prefix, context.options()["ignored_files"].get<Regex>(), cursor_pos); }
    });
    cm.register_commands({ "edit", "e" }, edit<false>, CommandFlags::None, filename_completer);
    cm.register_commands({ "edit!", "e!" }, edit<true>, CommandFlags::None, filename_completer);
    cm.register_commands({ "write", "w" }, write_buffer, CommandFlags::None, filename_completer);
    cm.register_commands({ "writeall", "wa" }, write_all_buffers);
    cm.register_commands({ "quit", "q" }, quit<false>);
    cm.register_commands({ "quit!", "q!" }, quit<true>);
    cm.register_command("wq", write_and_quit<false>);
    cm.register_command("wq!", write_and_quit<true>);

    PerArgumentCommandCompleter buffer_completer({
        [](const Context& context, CompletionFlags flags, const String& prefix, ByteCount cursor_pos)
        { return BufferManager::instance().complete_buffername(prefix, cursor_pos); }
    });
    cm.register_commands({ "buffer", "b" }, show_buffer, CommandFlags::None, buffer_completer);
    cm.register_commands({ "delbuf", "db" }, delete_buffer<false>, CommandFlags::None, buffer_completer);
    cm.register_commands({ "delbuf!", "db!" }, delete_buffer<true>, CommandFlags::None, buffer_completer);
    cm.register_commands({ "namebuf", "nb" }, set_buffer_name);

    auto get_highlighters = [](const Context& c) -> HighlighterGroup& { return c.window().highlighters(); };
    cm.register_commands({ "addhl", "ah" }, add_highlighter, CommandFlags::None, group_add_completer<HighlighterRegistry>(get_highlighters));
    cm.register_commands({ "rmhl", "rh" }, rm_highlighter, CommandFlags::None, group_rm_completer(get_highlighters));
    cm.register_commands({ "defhl", "dh" }, define_highlighter);

    cm.register_command("hook", add_hook);
    cm.register_command("rmhooks", rm_hooks);

    cm.register_command("source", exec_commands_in_file, CommandFlags::None, filename_completer);

    cm.register_command("exec", exec_string);
    cm.register_command("eval", eval_string);
    cm.register_command("menu", menu);
    cm.register_command("info", info);
    cm.register_command("try",  try_catch);
    cm.register_command("reg", set_register);

    cm.register_command("def",  define_command);
    cm.register_command("decl", declare_option);

    cm.register_command("echo", echo_message);
    cm.register_command("debug", write_debug_message);

    cm.register_command("set", set_option, CommandFlags::None,
                        [](const Context& context, CompletionFlags, CommandParameters params, size_t token_to_complete, ByteCount pos_in_token)
                        {
                            if (token_to_complete == 0)
                            {
                                CandidateList res;
                                for (auto scope : { "global", "buffer", "window" })
                                {
                                    if (params.size() == 0 or prefix_match(scope, params[0].substr(0_byte, pos_in_token)))
                                        res.emplace_back(scope);
                                }
                                return res;
                            }
                            else if (token_to_complete == 1)
                            {
                                OptionManager& options = get_options(params[0], context);
                                return options.complete_option_name(params[1], pos_in_token);
                            }
                            return CandidateList{};
                        } );

    cm.register_commands({ "colalias", "ca" }, define_color_alias);
    cm.register_commands({ "nameclient", "nc" }, set_client_name);

    cm.register_command("cd", change_working_directory, CommandFlags::None, filename_completer);
    cm.register_command("map", map_key);
}
}
