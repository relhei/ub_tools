# Python 2 module
# -*- coding: utf-8 -*-


from __future__ import print_function
from email.mime.text import MIMEText
import ConfigParser
import datetime
import glob
import os
import process_util
import smtplib
import socket
import struct
import sys
import tarfile
import time


default_email_sender = "unset_email_sender@ub.uni-tuebingen.de"
default_email_recipient = "johannes.ruscheinski@uni-tuebingen.de"
default_config_file_dir = "/var/lib/tuelib/cronjobs/"


def SendEmail(subject, msg, sender=None, recipient=None):
    subject += " (from: " + socket.gethostname() + ")"
    if sender is None:
        sender = default_email_sender
    if recipient is None:
        recipient = default_email_recipient
    try:
        config = LoadConfigFile(no_error=True)
        server_address  = config.get("SMTPServer", "server_address")
        server_user     = config.get("SMTPServer", "server_user")
        server_password = config.get("SMTPServer", "server_password")
    except Exception as e:
        print("failed to read config file! (" + str(e) + ")", file=sys.stderr)
        sys.exit(-1)

    message = MIMEText(msg, 'plain', 'utf-8')
    message["Subject"] = subject
    message["From"] = sender
    message["To"] = recipient
    server = smtplib.SMTP(server_address)
    try:
        server.ehlo()
        server.starttls()
        server.login(server_user, server_password)
        server.sendmail(sender, [recipient], message.as_string())
    except Exception as e:
        print("Failed to send your email: " + str(e), file=sys.stderr)
        sys.exit(-1)
    server.quit()


def Error(msg):
    print(sys.argv[0] + ": " + msg, file=sys.stderr)
    SendEmail("Script error (script: " + os.path.basename(sys.argv[0]) + ")!", msg)
    sys.exit(1)


def Warning(msg):
    print(sys.argv[0] + ": " + msg, file=sys.stderr)


# @brief Copy the contents, in order, of "files" into "target".
# @return True if we succeeded, else False.
def ConcatenateFiles(files, target):
    return process_util.Exec("/bin/cat", files, new_stdout=target) == 0


# Fails if "source" does not exist or if "link_name" exists and is not a symlink.
# Calls Error() upon failure and aborts the program.
def SafeSymlink(source, link_name):
    try:
        os.lstat(source) # Throws an exception if "source" does not exist.
        if os.access(link_name, os.F_OK) != 0:
            if os.path.islink(link_name):
                os.unlink(link_name)
            else:
                Error("in util.SafeSymlink: trying to create a symlink to \"" + link_name
                      + "\" which is an existing non-symlink file!")
        os.symlink(source, link_name)
    except Exception as e:
        Error("in util.SafeSymlink: os.symlink() failed: " + str(e))


# @return The absolute path of the file "link_name" points to.
def ResolveSymlink(link_name):
    resolved_path = os.readlink(link_name)
    if resolved_path[0] == '/':  # Absolute path?
        return resolved_path
    dirname = os.path.dirname(link_name)
    if not dirname:
        dirname = os.getcwdu()
    return os.path.join(dirname, resolved_path)


# @return True if "path" has been successfully unlinked, else False.
def Remove(path):
    try:
        os.unlink(path)
        return True
    except:
        return False


# @brief  Looks for "prefix.timestamp", and if found reads the timestamp stored in it.
# @param  prefix  This followed by ".timestamp" will be used as the name of the timestamp file.
#                 If this is None, the name of the Python executable minus the last three characters
#                 (hopefully ".py") will be used.
# @note   Timestamps are in seconds since the UNIX epoch.
# @return Either the value found in the timestamp file or the UNIX epoch, that is 0.
def ReadTimestamp(prefix = None):
    if prefix is None:
        prefix = os.path.basename(sys.argv[0])[:-3]
    timestamp_filename = prefix + ".timestamp"
    if not os.access(timestamp_filename, os.R_OK):
        return 0
    with open(timestamp_filename, "rb") as timestamp_file:
        (timestamp, ) = struct.unpack('d', timestamp_file.read())
        return timestamp


# @param timestamp  A float.
# @param prefix     This combined with ".timestamp" will be the name of the file that will be written.
# @param timestamp  A value in seconds since the UNIX epoch or None, in which case the current time
#                   will be used
def WriteTimestamp(prefix=None, timestamp=None):
    if prefix is None:
        prefix = os.path.basename(sys.argv[0])[:-3]
    elif type(timestamp) is not float:
        raise TypeError("timestamp argument of WriteTimestamp() must be of type \"float\"!")
    if timestamp is None:
        timestamp = time.time()
    timestamp_filename = prefix + ".timestamp"
    Remove(timestamp_filename)
    with open(timestamp_filename, "wb") as timestamp_file:
        timestamp_file.write(struct.pack('d', timestamp))


def LoadConfigFile(path=None, no_error=False):
    if path is None: # Take script name w/ "py" extension replaced by "conf".
        path = default_config_file_dir + os.path.basename(sys.argv[0])[:-2] + "conf"
    try:
        if not os.access(path, os.R_OK):
            if no_error:
                raise OSError("in util.LoadConfigFile: can't open \"" + path + "\" for reading!")
            Error("in util.LoadConfigFile: can't open \"" + path + "\" for reading!")
        config = ConfigParser.ConfigParser()
        config.read(path)
        return config
    except Exception as e:
        if no_error:
            raise e
        Error("in util.LoadConfigFile: failed to load the config file from \"" + path + "\"! (" + str(e) + ")")


# Extracts the typical 3 files from a gizipped tar archive.
# @param name_prefix  If not None, this will be prepended to the names of the extracted files
# @return The list of names of the extracted files in the order: title data, superior data, norm data
def ExtractAndRenameBSZFiles(gzipped_tar_archive, name_prefix = None):
    def ExtractAndRenameMember(tar_file, member, new_name):
        Remove(member)
        tar_file.extract(member)
        Remove(new_name)
        os.rename(member, new_name)

    if name_prefix is None:
        name_prefix = ""
    tar_file = tarfile.open(gzipped_tar_archive, "r:gz")
    members = tar_file.getnames()
    current_date_str = datetime.datetime.now().strftime("%d%m%y")
    for member in members:
        if member.endswith("a001.raw"):
            ExtractAndRenameMember(tar_file, member, name_prefix + "TitelUndLokaldaten-" + current_date_str + ".mrc")
        elif member.endswith("b001.raw"):
            ExtractAndRenameMember(tar_file, member,
                                   name_prefix + "ÜbergeordneteTitelUndLokaldaten-" + current_date_str + ".mrc")
        elif member.endswith("c001.raw"):
            ExtractAndRenameMember(tar_file, member, name_prefix + "Normdaten-" + current_date_str + ".mrc")
        else:
            Error("in util.ExtractAndRenameBSZFiles: Unknown member \"" + member + "\" in archive \""
                  + gzipped_tar_archive + "\"!")
    return [name_prefix + "TitelUndLokaldaten-" + current_date_str + ".mrc",
            name_prefix + "ÜbergeordneteTitelUndLokaldaten-" + current_date_str + ".mrc",
            name_prefix + "Normdaten-" + current_date_str + ".mrc"]


def IsExecutableFile(executable_candidate):
    return os.path.isfile(executable_candidate) and os.access(executable_candidate, os.X_OK)


# @return the path to the executable "executable_candidate" if found, or else None
def Which(executable_candidate):

    dirname, basename = os.path.split(executable_candidate)
    if dirname:
        if IsExecutableFile(executable_candidate):
            return executable_candidate
    else:
        for path in os.environ["PATH"].split(":"):
            full_name = os.path.join(path, executable_candidate)
            if IsExecutableFile(full_name):
                return full_name

    return None


# Strips the path and an optional extension from "reference_file_name" and appends ".log"
# and prepends "log_directory".
# @return The complete path for the log file name.
def MakeLogFileName(reference_file_name, log_directory):
    if not log_directory.endswith("/"):
        log_directory += "/"
    last_dot_pos = reference_file_name.rfind(".")
    if last_dot_pos == -1:
        log_file_name = log_directory + os.path.basename(reference_file_name) + ".log"
    else:
        log_file_name = log_directory + os.path.basename(reference_file_name[:last_dot_pos]) + "log"


# @return the most recent file matching "file_name_glob" or None if there were no matching files.
def getMostRecentFileMatchingGlob(file_name_glob):
    most_recent_matching_name = None
    most_recent_mtime = None
    for name in glob.glob(file_name_glob):
        if not most_recent_matching_name:
            most_recent_matching_name = name
        else:
            stat_buf = os.stat(name)
            if most_recent_mtime is None or stat_buf.st_mtime > most_recent_mtime:
                most_recent_matching_name = name
                most_recent_mtime = stat_buf.st_mtime

    return most_recent_matching_name
