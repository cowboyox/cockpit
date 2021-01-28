/*
 * This file is part of Cockpit.
 *
 * Copyright (C) 2016 Red Hat, Inc.
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

import cockpit from "cockpit";
import React from "react";

import '../lib/patternfly/patternfly-cockpit.scss';
import "../../node_modules/@patternfly/patternfly/components/Page/page.css";
import "table.css";

import { show_modal_dialog } from "cockpit-components-dialog.jsx";

import { PatternDialogBody } from "./react-demo-dialog.jsx";
import { showCardsDemo } from "./react-demo-cards.jsx";
import { showFileAcDemo } from "./react-demo-file-autocomplete.jsx";

/* -----------------------------------------------------------------------------
  Modal Dialog
  -----------------------------------------------------------------------------
 */

var lastAction = "";

var onDialogStandardClicked = function(mode) {
    lastAction = mode;
    var dfd = cockpit.defer();
    dfd.notify("Starting something long");
    if (mode == 'steps') {
        var interval;
        var count = 0;
        interval = window.setInterval(function() {
            count += 1;
            dfd.notify("Step " + count);
        }, 500);
        window.setTimeout(function() {
            window.clearTimeout(interval);
            dfd.resolve();
        }, 5000);
        dfd.promise.cancel = function() {
            window.clearTimeout(interval);
            dfd.notify("Canceling");
            window.setTimeout(function() {
                dfd.reject("Action canceled");
            }, 1000);
        };
    } else if (mode == 'reject') {
        dfd.reject("Some error occurred");
    } else {
        dfd.resolve();
    }
    return dfd.promise;
};

var onDialogDone = function(success) {
    var result = success ? "successful" : "Canceled";
    var action = success ? lastAction : "no action";
    document.getElementById("demo-dialog-result").textContent = "Dialog closed: " + result + "(" + action + ")";
};

var onStandardDemoClicked = function(staticError) {
    var dialogProps = {
        title: "This shouldn't be seen",
        body: React.createElement(PatternDialogBody, { clickNested: onStandardDemoClicked }),
    };
    // also test modifying properties in subsequent render calls
    var footerProps = {
        actions: [
            {
                clicked: onDialogStandardClicked.bind(null, 'standard action'),
                caption: "OK",
                style: 'primary',
            },
            {
                clicked: onDialogStandardClicked.bind(null, 'dangerous action'),
                caption: "Danger",
                style: 'danger',
            },
            {
                clicked: onDialogStandardClicked.bind(null, 'steps'),
                caption: "Wait",
            },
        ],
        static_error: staticError,
        dialog_done: onDialogDone,
    };
    var dialogObj = show_modal_dialog(dialogProps, footerProps);
    // if this failed, exit (trying to create a nested dialog)
    if (!dialogObj)
        return;
    footerProps.actions.push(
        {
            clicked: onDialogStandardClicked.bind(null, 'reject'),
            caption: "Error",
            style: 'primary',
        });
    dialogObj.setFooterProps(footerProps);
    dialogProps.title = "Example React Dialog";
    dialogObj.setProps(dialogProps);
};

document.addEventListener("DOMContentLoaded", function() {
    document.getElementById('demo-show-dialog').addEventListener("click", onStandardDemoClicked.bind(null, null), false);
    document.getElementById('demo-show-error-dialog').addEventListener("click", onStandardDemoClicked.bind(null, 'Some static error'), false);

    /* -----------------------------------------------------------------------------
      Listing Pattern
      -----------------------------------------------------------------------------
     */
    // File autocomplete
    showFileAcDemo(document.getElementById('demo-file-ac'));

    // Cards
    showCardsDemo(document.getElementById('demo-cards'));
});
