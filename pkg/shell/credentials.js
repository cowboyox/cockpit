/*
 * This file is part of Cockpit.
 *
 * Copyright (C) 2015 Red Hat, Inc.
 *
 * Cockpit is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * Cockpit is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Cockpit; If not, see <http://www.gnu.org/licenses/>.
 */

define([
    "jquery",
    "base1/cockpit",
    "base1/patterns",
], function($, cockpit) {
    "use strict";

    var _ = cockpit.gettext;

    /* The button to deauthorize cockpit */
    $("#credentials-authorize button").on("click", function(ev) {
        $("#credentials-authorize").remove();
        cockpit.drop_privileges(false);
        ev.preventDefault();
    });

    function Keys() {
        var self = this;

        self.path = cockpit.user["home"] + "/.ssh";
        self.items = { };

        var files = { };
        var watch = cockpit.channel({ payload: "fslist1", path: self.path });

        var process = null;
        var wait = null;
        var timer = null;

        agent_list();

        function fire_changed() {
            if (!wait) {
                wait = window.setTimeout(function() {
                    wait = null;
                    $(self).triggerHandler("changed");
                }, 100);
            }
        }

        function key_update(data, file, label) {
            var parts = data.split(" ");
            if (parts[0].slice(0, 4) !== "ssh-")
                return;
            var id = parts[1];
            var key = self.items[id];
            if (!key)
                key = self.items[id] = { id: id };
            if (file !== undefined)
                key.file = file;
            if (label)
                key.label = label;
            if (!key.label) /* use comment as fallback */
                key.label = parts.slice(3).join(" ");
            fire_changed();
            return id;
        }

        function key_purge(key) {
            if (key && !key.loaded && !key.file) {
                delete self.items[key.id];
                fire_changed();
            }
        }

        function file_deleted(name) {
            var key, file = files[name];
            if (file) {
                delete files[name];
                if (file.id) {
                    key = self.items[file.id];
                    if (key) {
                        key.file = null;
                        key_purge();
                    }
                }
            }
        }

        function file_read(name) {
            var file = files[name];
            if (file) {
                file.read()
                    .done(function(data) {
                        var key, old = file.id;
                        file.id = key_update(data, file, name.slice(0, -4));
                        if (old && old != file.id) {
                            key = self.items[old];
                            if (key) {
                                key.file = null;
                                key_purge(key);
                            }
                        }
                    })
                    .fail(function(ex) {
                        if (ex.problem !== "cancelled")
                            console.warn("couldn't read ssh key file: " + ex);
                    });
            }
        }

        function file_updated(name) {
            var file = files[name];
            if (!file)
                file = files[name] = cockpit.file(self.path + "/" + name);
            if (!file.timeout) {
                file.timeout = window.setTimeout(function() {
                    file.timeout = null;
                    file_read(name);
                }, 20);
            }
        }

        $(watch).on("message", function(ev, payload) {
            var item = JSON.parse(payload);
            var name = item.path;
            if (name && name.indexOf("/") === -1 && name.slice(-4) === ".pub") {
                if (item.event === "present" ||item.event === "created" || item.event === "changed")
                    file_updated(name);
                else if (item.event === "deleted")
                    file_deleted(name);
            }
        });

        function agent_update(data) {
            var key, id, seen = { };
            var lines = data.split("\n");
            lines.forEach(function(line) {
                id = key_update(line);
                if (id)
                    seen[id] = true;
            });
            for (id in self.items) {
                key = self.items[id];
                key.loaded = (id in seen);
                key_purge(key);
            }
        }

        function agent_list() {
            timer = null;
            process = cockpit.spawn(["ssh-add", "-L"], { err: "message" })
                .done(function(data) {
                    agent_update(data);
                    timer = window.setTimeout(agent_list, 5000);
                })
                .fail(function(ex) {
                    console.log("couldn't list agent keys: " + ex);
                });
        }

        self.password = function(path, old_pass, new_pass) {
            var old_exps = [ /.*Enter old passphrase: $/ ];
            var new_exps = [ /.*Enter new passphrase.*/, /.*Enter same passphrase again: $/ ];
            var bad_exps = [ /.*failed: passphrase is too short.*/ ];

            var dfd = $.Deferred();
            var buffer = "";
            var sent_new = false;
            var failure = _("No such file or directory");
            var i;

            var timeout = window.setTimeout(function() {
                failure = _("Prompting via ssh-keygen timed out");
                proc.close("terminated");
            }, 10 * 1000);

            var proc = cockpit.spawn(["ssh-keygen", "-f", path, "-p"],
                    { pty: true, environ: [ "LC_ALL=C" ], err: "out" })
                .always(function() {
                    window.clearInterval(timeout);
                })
                .done(function() {
                    dfd.resolve();
                })
                .fail(function(ex) {
                    if (ex.constructor.name == "ProcessError")
                        ex = new Error(failure);
                    dfd.reject(ex);
                })
                .stream(function(data) {
                    buffer += data;
                    for (i = 0; i < old_exps.length; i++) {
                        if (old_exps[i].test(buffer)) {
                            buffer = "";
                            failure = _("Old password not accepted");
                            proc.input(old_pass + "\n", true);
                            return;
                        }
                    }

                    for (i = 0; i < new_exps.length; i++) {
                        if (new_exps[i].test(buffer)) {
                            buffer = "";
                            proc.input(new_pass + "\n", true);
                            failure = _("Failed to change password");
                            sent_new = true;
                            return;
                        }
                    }

                    for (i = 0; sent_new && i < bad_exps.length; i++) {
                        if (bad_exps[i].test(buffer)) {
                            failure = _("New password was not accepted");
                            return;
                        }
                    }
                });

            return dfd.promise();
        };

        self.load = function(path, password) {
            var ask_exp =  /.*Enter passphrase for .*/;
            var perm_exp = /.*UNPROTECTED PRIVATE KEY FILE.*/;
            var bad_exp = /.*Bad passphrase.*/;

            var dfd = $.Deferred();
            var buffer = "";
            var failure = _("Not a valid private key");

            var timeout = window.setTimeout(function() {
                failure = _("Prompting via ssh-add timed out");
                proc.close("terminated");
            }, 10 * 1000);

            var proc = cockpit.spawn(["ssh-add", path],
                    { pty: true, environ: [ "LC_ALL=C" ], err: "out" })
                .always(function() {
                    window.clearInterval(timeout);
                })
                .done(function() {
                    dfd.resolve();
                })
                .fail(function(ex) {
                    if (ex.constructor.name == "ProcessError")
                        ex = new Error(failure);
                    dfd.reject(ex);
                })
                .stream(function(data) {
                    buffer += data;
                    if (perm_exp.test(buffer)) {
                        failure = _("Invalid file permissions");
                        buffer = "";
                    } else if (ask_exp.test(buffer)) {
                        buffer = "";
                        failure = _("Password not accepted");
                        proc.input(password + "\n", true);
                    } else if (bad_exp.test(buffer)) {
                        buffer = "";
                        proc.input("\n", true);
                    }
                });

            return dfd.promise();
        };

        self.close = function close() {
            watch.close();
            process.close();
            window.clearTimeout(timer);
            timer = null;
            window.clearTimeout(wait);
            wait = null;
        };
    }

    $("#credentials-dialog").on("show.bs.modal", function() {
        var keys = new Keys();

        $(keys).on("changed", function() {
            var key, id, row, rows = { };
            var body = $("#credentials-dialog tbody");

            body.find("tr[data-id]").each(function(i, el) {
                row = $(el);
                rows[row.attr("data-id")] = row;
            });

            for (id in keys.items) {
                if (!(id in rows)) {
                    row = rows[id] = $("<tr><th></th><td></td><td><div class='btn-onoff'></div></td></tr>");
                    row.attr("data-id", id).show().onoff();
                    body.append(row);
                }
            }

            for (id in rows) {
                row = rows[id];
                key = keys.items[id];
                if (key) {
                    row.find("th").text(key.label);
                    row.find(".btn-onoff")
                        .onoff("value", key.loaded)
                        .onoff("disabled", !key.file);
                } else {
                    row.remove();
                }
            }

        });

        $(this).on("hide.bs.modal", function() {
            keys.close();
        });
    });
});
