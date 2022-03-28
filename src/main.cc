#include "common.hh"
#include "process.hh"

#if defined(_WIN32)
  #include "win.hh"
#elif defined(__APPLE__)
  #include "mac.hh"
#elif defined(__linux__)
  #include "linux.hh"
#endif

#if defined(_WIN32)
  #include <io.h>
  #define ISATTY _isatty
  #define FILENO _fileno
#else
  #include <unistd.h>
  #include <sys/wait.h>
  #define ISATTY isatty
  #define FILENO fileno
#endif

using namespace Operator;

std::function<void(int)> shutdownHandler;
void signalHandler(int signal) { shutdownHandler(signal); }

//
// the MAIN macro provides a cross-platform program entry point.
// it makes argc and argv uniformly available. It provides "instanceId"
// which on windows is hInstance, on mac and linux this is just an int.
//
MAIN {

  App app(instanceId);

  //
  // SETTINGS and DEBUG are compile time variables provided by the compiler.
  //
  constexpr auto _settings = STR_VALUE(SETTINGS);
  constexpr auto _debug = DEBUG;

  //
  // Prepare to forward commandline arguments to the main and render processes.
  // TODO (@heapwolf): make this url encoded, this way is a little weird.
  //
  auto cwd = app.getCwd(argv[0]);
  appData = parseConfig(decodeURIComponent(_settings));

  std::string suffix = "";

  std::stringstream argvArray;
  std::stringstream argvForward;

  bool isCommandMode = false;
  bool isTest = false;

  int exitCode = 0;
  int c = 0;

  bool wantsVersion = false;
  bool wantsHelp = false;

  // TODO right now we forward a json parsable string as the args but this
  // isn't the most robust way of doing this. possible a URI-encoded query
  // string would be more in-line with how everything else works.
  for (auto const arg : std::span(argv, argc)) {
    argvArray
      << "'"
      << replace(std::string(arg), "'", "\'")
      << (c++ < argc ? "', " : "'");

    bool helpRequested =
      (std::string(arg).find("--help") == 0) ||
      (std::string(arg).find("-help") == 0) ||
      (std::string(arg).find("-h") == 0);

    bool versionRequested =
      (std::string(arg).find("--version") == 0) ||
      (std::string(arg).find("-version") == 0) ||
      (std::string(arg).find("-v") == 0) ||
      (std::string(arg).find("-V") == 0);

    if (helpRequested) {
      wantsHelp = true;
    }

    if (versionRequested) {
      wantsVersion = true;
    }

    if (std::string(arg).find("--test") == 0) {
      suffix = "-test";
      isTest = true;
    } else if (c >= 2 && std::string(arg).find("-") != 0) {
      isCommandMode = true;
    }

    if (helpRequested || versionRequested) {
      isCommandMode = true;
    }

    if (helpRequested) {
      argvForward << " " << "help --warn-arg-usage=" << std::string(arg);
    } else if (versionRequested) {
      argvForward << " " << "version --warn-arg-usage=" << std::string(arg);
    } else if (c > 1 || isCommandMode) {
      argvForward << " " << std::string(arg);
    }
  }

  #if DEBUG == 1
    appData["name"] += "-dev";
    appData["title"] += "-dev";
  #endif

  appData["name"] += suffix;
  appData["title"] += suffix;

  argvForward << " --version=" << appData["version"];
  argvForward << " --name=" << appData["name"];

  #if DEBUG == 1
    argvForward << " --debug=1";
  #endif

  std::stringstream env;
  for (auto const &envKey : split(appData["env"], ',')) {
    auto cleanKey = trim(envKey);
    auto envValue = getEnv(cleanKey.c_str());

    env << std::string(
      cleanKey + "=" + encodeURIComponent(envValue) + "&"
    );
  }

  auto cmd = appData[platform.os + "_cmd"];
  if (cmd[0] == '.') {
    auto index = cmd.find_first_of('.');
    auto executable = cmd.substr(0, index);
    auto absPath = fs::path(cwd) / fs::path(executable);

    cmd = pathToString(absPath) + cmd.substr(index);
  }

  if (isCommandMode) {
    argvForward << " --op-current-directory=" << fs::current_path();

    Process process(
      cmd + argvForward.str(),
      cwd,
      [&](std::string const &out) {
        Parse cmd(out);

        if (cmd.name != "exit") {
          std::cout << decodeURIComponent(cmd.get("value")) << std::endl;
        } else if (cmd.name == "exit") {
          exitCode = stoi(cmd.get("value"));
          exit(exitCode);
        }
      },
      [](std::string const &out) {
        std::cerr << out;
      }
    );

    shutdownHandler = [&](int signum) {
      auto pid = process.getPID();
      process.kill(pid);
      exit(signum);
    };

    #ifndef _WIN32
      signal(SIGHUP, signalHandler);
    #endif

    signal(SIGINT, signalHandler);

    return exitCode;
  }

  //
  // # Windows
  //
  // The Window constructor takes the app instance as well as some static
  // variables used during setup, these options can all be overridden later.
  //
  Window w0(app, WindowOptions {
    .resizable = true,
    .frameless = false,
    .canExit = true,
    .height = std::stoi(appData["height"]),
    .width = std::stoi(appData["width"]),
    .index = 0,
    .debug = _debug,
    .isTest = isTest,
    .forwardConsole = appData["forward_console"] == "true",
    .cwd = cwd,
    .executable = appData["executable"],
    .title = appData["title"],
    .version = appData["version"],
    .argv = argvArray.str(),
    .preload = gPreloadDesktop,
    .env = env.str()
  });

  if (w0.webviewFailed) {
    argvForward << " --webviewFailed";
  }

  //
  // The second window is used for showing previews or progress, so it can
  // be frameless and prevent resizing, etc. it gets the same preload so
  // that we can communicate with it from the main process.
  //
  Window w1(app, WindowOptions {
    .resizable = true,
    .canExit = false,
    .height = 120,
    .width = 350,
    .index = 1,
    .debug = _debug,
    .isTest = isTest,
    .forwardConsole = appData["forward_console"] == "true",
    .cwd = cwd,
    .executable = appData["executable"],
    .title = appData["title"],
    .version = appData["version"],
    .argv = argvArray.str(),
    .preload = gPreloadDesktop,
    .env = env.str()
  });

  //
  // # Main -> Render
  // Launch the main process and connect callbacks to the stdio and stderr pipes.
  //
  auto onStdOut = [&](auto out) {
    //
    // ## Dispatch
    // Messages from the main process may be sent to the render process. If they
    // are parsable commands, try to do something with them, otherwise they are
    // just stdout and we can write the data to the pipe.
    //
    app.dispatch([&, out] {
      Parse cmd(out);

      auto &w = cmd.index == 0 ? w0 : w1;
      auto seq = cmd.get("seq");
      auto value = cmd.get("value");

      if (cmd.name == "title") {
        w.setTitle(seq, decodeURIComponent(value));
        return;
      }

      if (cmd.name == "restart") {
        app.restart();
        return;
      }

      if (cmd.name == "show") {
        w.show(seq);
        return;
      }

      if (cmd.name == "hide") {
        w.hide(seq);
        return;
      }

      if (cmd.name == "navigate") {
        w.navigate(seq, decodeURIComponent(value));
        return;
      }

      if (cmd.name == "size") {
        int width = std::stoi(cmd.get("width"));
        int height = std::stoi(cmd.get("height"));
        w.setSize(seq, width, height, 0);
        return;
      }

      if (cmd.name == "getScreenSize") {
        auto size = w.getScreenSize();

        std::string value(
          "{"
            "\"width\":" + std::to_string(size.width) + ","
            "\"height\":" + std::to_string(size.height) + ""
          "}"
        );

        w.onMessage(resolveToMainProcess(seq, "0", encodeURIComponent(value)));
        return;
      }

      if (cmd.name == "menu") {
        w.setSystemMenu(seq, decodeURIComponent(value));
        return;
      }

      if (cmd.name == "external") {
        w.openExternal(decodeURIComponent(value));
        if (seq.size() > 0) {
          w.onMessage(resolveToMainProcess(seq, "0", "null"));
        }
        return;
      }

      if (cmd.name == "exit") {
        try {
          exitCode = std::stoi(value);
        } catch (...) {
        }

        w.exit(exitCode);

        if (seq.size() > 0) {
          w.onMessage(resolveToMainProcess(seq, "0", "null"));
        }
        return;
      }

      if (cmd.name == "resolve") {
        w.eval(resolveToRenderProcess(seq, cmd.get("state"), value));
        return;
      }

      if (cmd.name == "send") {
        w.eval(emitToRenderProcess(
          decodeURIComponent(cmd.get("event")),
          value
        ));
        return;
      }

      if (cmd.name == "stdout") {
        writeToStdout(decodeURIComponent(value));
      }
    });
  };

  auto onStdErr = [&](auto err) {
    std::cerr << err << std::endl;
  };

  Process process(
    cmd + argvForward.str(),
    cwd,
    onStdOut,
    onStdErr
  );

  //
  // # Render -> Main
  // Send messages from the render processes to the main process.
  // These may be similar to how we route the messages from the
  // main process but different enough that duplication is ok. This
  // callback doesnt need to dispatch because it's already in the
  // main thread.
  //
  auto onMessage = [&](auto out) {
    Parse cmd(out);

    auto &w = cmd.index == 0 ? w0 : w1;

    if (cmd.name == "title") {
      w.setTitle(
        cmd.get("seq"),
        decodeURIComponent(cmd.get("value"))
      );
      return;
    }

    if (cmd.name == "exit") {
      try {
        exitCode = std::stoi(decodeURIComponent(cmd.get("value")));
      } catch (...) {
      }

      w.exit(exitCode);
      return;
    }

    if (cmd.name == "hide") {
      w.hide("");
      return;
    }

    if (cmd.name == "inspect") {
      w.showInspector();
      return;
    }

    if (cmd.name == "external") {
      w.openExternal(decodeURIComponent(cmd.get("value")));
      return;
    }

    if (cmd.name == "dialog") {
      bool isSave = cmd.get("type").compare("save") == 0;
      bool allowDirs = cmd.get("allowDirs").compare("true") == 0;
      bool allowFiles = cmd.get("allowFiles").compare("true") == 0;
      bool allowMultiple = cmd.get("allowMultiple").compare("true") == 0;
      std::string defaultName = decodeURIComponent(cmd.get("defaultName"));
      std::string defaultPath = decodeURIComponent(cmd.get("defaultPath"));
      std::string title = decodeURIComponent(cmd.get("title"));

      w.openDialog(
        cmd.get("seq"),
        isSave,
        allowDirs,
        allowFiles,
        allowMultiple,
        defaultPath,
        title,
        defaultName
      );

      return;
    }

    if (cmd.name == "context") {
      auto seq = cmd.get("seq");
      auto value = decodeURIComponent(cmd.get("value"));
      w.setContextMenu(seq, value);
      return;
    }

    //
    // Everything else can be forwarded to the main process.
    // The protocol requires messages must be terminated by a newline.
    //
    process.write(out);
  };

  w0.onMessage = onMessage;
  w1.onMessage = onMessage;

  //
  // # Exiting
  //
  // When a window or the app wants to exit,
  // we clean up the windows and the main process.
  //
  shutdownHandler = [&](int code) {
    auto pid = process.getPID();
    process.kill(pid);

    w0.kill();
    w1.kill();
    app.kill();
    exit(code);
  };

  app.onExit = shutdownHandler;
  w0.onExit = shutdownHandler;
  w1.onExit = shutdownHandler;

  //
  // If this is being run in a terminal/multiplexer
  //
  #ifndef _WIN32
    signal(SIGHUP, signalHandler);
  #endif

  signal(SIGINT, signalHandler);

  //
  // # Event Loop
  // start the platform specific event loop for the main
  // thread and run it until it returns a non-zero int.
  //
  while(app.run() == 0);

  exit(exitCode);
}
