#include "session_app.h"

int main(int argc, char** argv) {
  crossdesk::session::Options opts;
  if (!crossdesk::session::ParseArgs(argc, argv, &opts)) {
    return 1;
  }
  crossdesk::session::SessionApp app(std::move(opts));
  return app.Run();
}
