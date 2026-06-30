// Shell ↔ Session message-type constants. Mirrors src/ipc/ipc_messages.h.

class MsgType {
  // shell → session
  static const bootstrap     = 'bootstrap';
  static const applySettings = 'apply_settings';
  static const requestClose  = 'request_close';
  static const sendFile      = 'send_file';

  // session → shell
  static const hello         = 'hello';
  static const state         = 'state';
  static const stats         = 'stats';
  static const fileProgress  = 'file_progress';
  static const clipboard     = 'clipboard';
  static const exit          = 'exit';
}
