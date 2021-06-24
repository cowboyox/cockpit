import fcntl
import glob
import json
import os
import random
import resource
import shutil
import socket
import subprocess
import sys
import tempfile
import time

TEST_DIR = os.path.normpath(os.path.dirname(os.path.realpath(os.path.join(__file__, ".."))))


def browser_path(browser, show_browser):
    if browser == "chromium":
        return browser_path_chromium(show_browser)
    elif browser == "firefox":
        return browser_path_firefox()
    else:
        raise SystemError("Unsupported browser")


def browser_path_firefox():
    """ Return path to Firefox browser """
    p = subprocess.check_output("which firefox-nightly || which firefox || true",
                                shell=True, universal_newlines=True).strip()
    if p:
        return p
    return None


def browser_path_chromium(show_browser):
    """Return path to chromium browser.

    Support the following locations:
     - /usr/lib*/chromium-browser/headless_shell (chromium-headless RPM)
     - "chromium-browser", "chromium", or "google-chrome"  in $PATH (distro package)
     - node_modules/chromium/lib/chromium/chrome-linux/chrome (npm install chromium)

    Exit with an error if none is found.
    """

    # If we want to have interactive chromium, we don't want to use headless_shell
    if not show_browser:
        g = glob.glob("/usr/lib*/chromium-browser/headless_shell")
        if g:
            return g[0]

    p = subprocess.check_output("which chromium-browser || which chromium || which google-chrome || true",
                                shell=True, universal_newlines=True).strip()
    if p:
        return p

    p = os.path.join(os.path.dirname(TEST_DIR), "node_modules/chromium/lib/chromium/chrome-linux/chrome")
    if os.access(p, os.X_OK):
        return p

    return None


def jsquote(str):
    return json.dumps(str)


class CDP:
    def __init__(self, lang=None, verbose=False, trace=False, inject_helpers=[]):
        self.lang = lang
        self.timeout = 60
        self.valid = False
        self.verbose = verbose
        self.trace = trace
        self.inject_helpers = inject_helpers
        self.browser = os.environ.get("TEST_BROWSER", "chromium")
        self.show_browser = bool(os.environ.get("TEST_SHOW_BROWSER", ""))
        self.download_dir = tempfile.mkdtemp()
        self._driver = None
        self._browser = None
        self._browser_home = None
        self._browser_path = None
        self._cdp_port_lockfile = None

    def invoke(self, fn, **kwargs):
        """Call a particular CDP method such as Runtime.evaluate

        Use command() for arbitrary JS code.
        """
        trace = self.trace and not kwargs.get("no_trace", False)
        try:
            del kwargs["no_trace"]
        except KeyError:
            pass

        cmd = fn + "(" + json.dumps(kwargs) + ")"

        # frame support for Runtime.evaluate(): map frame name to
        # executionContextId and insert into argument object; this must not be quoted
        # see "Frame tracking" in cdp-driver.js for how this works
        if fn == 'Runtime.evaluate':
            cmd = "%s, contextId: getFrameExecId(%s)%s" % (cmd[:-2], jsquote(self.cur_frame), cmd[-2:])

        if trace:
            print("-> " + kwargs.get('trace', cmd))

        # avoid having to write the "client." prefix everywhere
        cmd = "client." + cmd
        res = self.command(cmd)
        if trace:
            if "result" in res:
                print("<- " + repr(res["result"]))
            else:
                print("<- " + repr(res))
        return res

    def command(self, cmd):
        if not self._driver:
            self.start()
        self._driver.stdin.write(cmd.encode("UTF-8"))
        self._driver.stdin.write(b"\n")
        self._driver.stdin.flush()
        line = self._driver.stdout.readline().decode("UTF-8")
        if not line:
            self.kill()
            raise RuntimeError("CDP broken")
        try:
            res = json.loads(line)
        except ValueError:
            print(line.strip())
            raise

        if "error" in res:
            if self.trace:
                print("<- raise %s" % str(res["error"]))
            raise RuntimeError(res["error"])
        return res["result"]

    def claim_port(self, port):
        f = None
        try:
            f = open(os.path.join(tempfile.gettempdir(), ".cdp-%i.lock" % port), "w")
            fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self._cdp_port_lockfile = f
            return True
        except (IOError, OSError):
            if f:
                f.close()
            return False

    def find_cdp_port(self):
        """Find an unused port and claim it through lock file"""

        for retry in range(100):
            # don't use the default CDP port 9222 to avoid interfering with running browsers
            port = random.randint(9223, 10222)
            if self.claim_port(port):
                return port
        else:
            raise RuntimeError("unable to find free port")

    def get_browser_path(self):
        if self._browser_path is None:
            self._browser_path = browser_path(self.browser, self.show_browser)

        return self._browser_path

    def browser_cmd(self, cdp_port, env):
        exe = self.get_browser_path()
        if not exe:
            raise SystemError(self.browser + " is not installed")

        if self.browser == "chromium":
            return [exe, "--headless" if not self.show_browser else "", "--disable-gpu", "--no-sandbox", "--disable-setuid-sandbox",
                    "--disable-namespace-sandbox", "--disable-seccomp-filter-sandbox",
                    "--disable-sandbox-denial-logging", "--disable-pushstate-throttle",
                    "--font-render-hinting=none",
                    "--v=0", "--window-size=1920x1200", "--remote-debugging-port=%i" % cdp_port, "about:blank"]
        elif self.browser == "firefox":
            subprocess.Popen([exe, "--headless", "--no-remote", "-CreateProfile", "blank"], env=env).communicate()
            profile = glob.glob(os.path.join(self._browser_home, ".mozilla/firefox/*.blank"))[0]

            with open(os.path.join(profile, "user.js"), "w") as f:
                f.write("""
                    user_pref("remote.enabled", true);
                    user_pref("remote.frames.enabled", true);
                    user_pref("app.update.auto", false);
                    user_pref("datareporting.policy.dataSubmissionEnabled", false);
                    user_pref("toolkit.telemetry.reportingpolicy.firstRun", false);
                    user_pref("dom.disable_beforeunload", true);
                    user_pref("browser.download.dir", "{0}");
                    user_pref("browser.download.folderList", 2);
                    user_pref("signon.rememberSignons", false);
                    user_pref("dom.navigation.locationChangeRateLimit.count", 9999);
                    """.format(self.download_dir))

            with open(os.path.join(profile, "handlers.json"), "w") as f:
                f.write('{"defaultHandlersVersion":{"en-US":4},"mimeTypes":{"application/xz":{"action":0,"extensions":["xz"]}}}')

            cmd = [exe, "-P", "blank", "--window-size=1920,1200", "--remote-debugging-port=%i" % cdp_port, "--no-remote", "localhost"]
            if not self.show_browser:
                cmd.insert(3, "--headless")
            return cmd

    def start(self):
        environ = os.environ.copy()
        if self.lang:
            environ["LC_ALL"] = self.lang
        self.cur_frame = None

        # allow attaching to external browser
        cdp_port = None
        if "TEST_CDP_PORT" in os.environ:
            p = int(os.environ["TEST_CDP_PORT"])
            if self.claim_port(p):
                # can fail when a test starts multiple browsers; only show the first one
                cdp_port = p

        if not cdp_port:
            # start browser on a new port
            cdp_port = self.find_cdp_port()
            self._browser_home = tempfile.mkdtemp()
            environ = os.environ.copy()
            environ["HOME"] = self._browser_home
            environ["LC_ALL"] = "en_US.UTF-8"
            # this might be set for the tests themselves, but we must isolate caching between tests
            try:
                del environ["XDG_CACHE_HOME"]
            except KeyError:
                pass

            # sandboxing does not work in Docker container
            self._browser = subprocess.Popen(
                self.browser_cmd(cdp_port, environ), env=environ, close_fds=True,
                preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
            if self.verbose:
                sys.stderr.write("Started %s (pid %i) on port %i\n" % (self._browser_path, self._browser.pid, cdp_port))

        # wait for CDP to be up
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        for retry in range(3000):
            try:
                s.connect(('127.0.0.1', cdp_port))
                break
            except socket.error:
                time.sleep(0.1)
        else:
            raise RuntimeError('timed out waiting for browser to start')

        # now start the driver
        if self.trace:
            # enable frame/execution context debugging if tracing is on
            environ["TEST_CDP_DEBUG"] = "1"
        self._driver = subprocess.Popen(["{0}/{1}-cdp-driver.js".format(os.path.dirname(__file__), self.browser), str(cdp_port)],
                                        env=environ,
                                        stdout=subprocess.PIPE,
                                        stdin=subprocess.PIPE,
                                        close_fds=True)
        self.valid = True

        for inject in self.inject_helpers:
            with open(inject) as f:
                src = f.read()
            # HACK: injecting sizzle fails on missing `document` in assert()
            src = src.replace('function assert( fn ) {', 'function assert( fn ) { if (true) return true; else ')
            # HACK: sizzle tracks document and when we switch frames, it sees the old document
            # although we execute it in different context.
            if (self.browser == "firefox"):
                src = src.replace('context = context || document;', 'context = context || window.document;')
            self.invoke("Page.addScriptToEvaluateOnNewDocument", source=src, no_trace=True)

    def kill(self):
        self.valid = False
        self.cur_frame = None
        if self._driver:
            self._driver.stdin.close()
            self._driver.wait()
            self._driver = None

        shutil.rmtree(self.download_dir, ignore_errors=True)

        if self._browser:
            if self.verbose:
                sys.stderr.write("Killing browser (pid %i)\n" % self._browser.pid)
            try:
                self._browser.terminate()
            except OSError:
                pass  # ignore if it crashed for some reason
            self._browser.wait()
            self._browser = None
            shutil.rmtree(self._browser_home, ignore_errors=True)
            os.remove(self._cdp_port_lockfile.name)
            self._cdp_port_lockfile.close()

    def set_frame(self, frame):
        self.cur_frame = frame
        if self.trace:
            print("-> switch to frame %s" % frame)

    def get_js_log(self):
        """Return the current javascript console log"""

        if self.valid:
            # needs to be wrapped in Promise
            messages = self.command("Promise.resolve(messages)")
            return map(lambda m: "%s: %s" % tuple(m), messages)
        return []

    def read_log(self):
        """Returns an iterator that produces log messages one by one.

        Blocks if there are no new messages right now."""

        if not self.valid:
            yield []
            return

        while True:
            messages = self.command("waitLog()")
            for m in messages:
                yield m
