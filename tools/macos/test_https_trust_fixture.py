"""Portable lifecycle tests; no subprocess or keychain access is performed."""

import subprocess
import unittest
from unittest.mock import Mock, call, patch

import test_https_trust as fixture


class TemporaryTrustRootTests(unittest.TestCase):
    def setUp(self):
        self.run = Mock()
        self.root = fixture.TemporaryTrustRoot(self.run, "generated.pem", "generated-sha1")
        self.add = ("sudo", "-n", "security", "add-trusted-cert", "-d", "-k",
                    "/Library/Keychains/System.keychain")
        self.delete = call("sudo", "-n", "security", "delete-certificate", "-Z",
                           "generated-sha1", "/Library/Keychains/System.keychain")

    def test_policy_changes_replace_without_removing_certificate(self):
        policies = [
            ("-r", "trustRoot", "-p", "ssl"),
            ("-r", "trustRoot", "-p", "codeSign"),
            ("-r", "trustRoot", "-p", "ssl", "-s", "wrong.invalid"),
            ("-r", "deny"),
        ]
        for policy in policies:
            self.root.set(*policy)
        self.assertEqual(self.run.call_args_list,
                         [call(*self.add, *policy, "generated.pem") for policy in policies])
        self.root.cleanup()
        self.assertEqual(self.run.call_args_list[-2:],
                         [call(*self.add, "-r", "deny", "generated.pem"), self.delete])
        self.assertFalse(self.root.installed)

    def test_cleanup_without_install_does_nothing(self):
        self.root.cleanup()
        self.run.assert_not_called()

    def test_cleanup_is_idempotent(self):
        self.root.set("-r", "trustRoot", "-p", "ssl")
        self.root.cleanup()
        self.run.reset_mock()
        self.root.cleanup()
        self.run.assert_not_called()

    def test_partial_install_is_cleaned_up(self):
        self.run.side_effect = [subprocess.TimeoutExpired("security", 60), None, None]
        with self.assertRaises(subprocess.TimeoutExpired):
            self.root.set("-r", "trustRoot", "-p", "ssl")
        self.root.cleanup()
        self.assertEqual(self.run.call_args_list[-2:],
                         [call(*self.add, "-r", "deny", "generated.pem"), self.delete])

    def test_deny_failure_still_deletes_and_fails(self):
        self.root.set("-r", "trustRoot", "-p", "ssl")
        self.run.side_effect = [subprocess.TimeoutExpired("security", 60), None]
        with self.assertRaises(subprocess.TimeoutExpired):
            self.root.cleanup()
        self.assertEqual(self.run.call_args_list[-1], self.delete)

    def test_delete_failure_is_not_silenced(self):
        self.root.set("-r", "trustRoot", "-p", "ssl")
        self.run.side_effect = [None, subprocess.CalledProcessError(1, "security")]
        with self.assertRaises(subprocess.CalledProcessError):
            self.root.cleanup()
        self.assertTrue(self.root.installed)


class RunnerGuardTests(unittest.TestCase):
    def test_rejects_non_disposable_environments_before_any_commands(self):
        for platform, actions, runner in [
            ("darwin", "true", "self-hosted"),
            ("darwin", "true", ""),
            ("darwin", "false", "github-hosted"),
            ("win32", "true", "github-hosted"),
        ]:
            with self.subTest(platform=platform, actions=actions, runner=runner):
                with patch.object(fixture.sys, "platform", platform), \
                     patch.dict(fixture.os.environ, {"GITHUB_ACTIONS": actions,
                                                    "RUNNER_ENVIRONMENT": runner}), \
                     patch.object(fixture.subprocess, "run") as run:
                    with self.assertRaisesRegex(SystemExit, "GitHub-hosted macOS"):
                        fixture.main()
                    run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
