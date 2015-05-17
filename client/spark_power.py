import os
import sys
import time
import urllib, urllib2
import json
import signal
import traceback
import base64
import getpass

# Spark cloud addresses
AUTH_URL = "https://api.spark.io/oauth/token"
API_URL = "https://api.spark.io/v1/devices"

# Config file used by Spark CLI
SPARK_CONFIG="~/.spark/spark.config.json"

# Time limit for cloud API fetches
FETCH_TIMEOUT = 60

# Spark 'Access token' fetching --------------------------------

def newToken(username,password):
    '''Gets access token from oauth service, using name & password'''
    authhdr = base64.encodestring('spark:spark').rstrip()
    data = urllib.urlencode({
        "grant_type" : "password",
        "username" : username,
        "password" : password
        })
    req = urllib2.Request(AUTH_URL)
    req.add_header("Authorization", "Basic " + authhdr)
    req.add_header("Content-Type", "application/x-www-form-urlencoded")
    req.add_data(data)
    try:
        resp = urllib2.urlopen(req)
        rdata = resp.read()
    except urllib2.URLError as e:
        # print "Exception", e
        # print "Headers", e.headers
        rdata = e.read()
    return json.loads(rdata)

def loadConfig():
    '''Tries to find existing token in Spark config file'''
    cfgFile = os.path.expanduser(SPARK_CONFIG)
    if not os.path.isfile(cfgFile):
        return None
    with open(cfgFile,"r") as fp:
        jcfg = json.load(fp)
    if 'access_token' not in jcfg:
        return None
    return jcfg

def saveConfig(email,access_token):
    '''Creates config file storing an access token for later use'''
    cfgFile = os.path.expanduser(SPARK_CONFIG)
    cfgDir = os.path.split(cfgFile)[0]
    if not os.path.isdir(cfgDir):
        os.makedirs(cfgDir)
    data = { 'username' : email, 'access_token' : access_token }
    with open(cfgFile, "w") as fp:
        json.dump(data, fp)

def getAccessToken():
    '''Reads access token from config file, or prompts to generate new one'''
    cfg = loadConfig()
    if cfg:
        return cfg['access_token']
    email = raw_input("Please enter Spark login email address: ")
    passwd = getpass.getpass("Please enter password: ")
    tknResp = newToken(email,passwd)
    if 'access_token' not in tknResp:
        for (k,v) in tknResp.items():
            print "%-20s: %s" % (k,v)
        sys.exit("ERROR: could not get access token")
    tkn = tknResp['access_token']
    saveConfig(email, tkn)
    return tkn

# Spark cloud API functions ------------------------------------

def alarmHandler(signum, frame):
    print "-- alarm handler called"
    raise IOError("Timeout exceeded")

def sparkGetVar(dev, access_token, varName):
    params = urllib.urlencode({ 'access_token' : access_token })
    req = urllib2.Request(API_URL + "/" + dev + "/" + varName + '?' + params)
    # print req.get_full_url()
    signal.alarm(60)
    resp = urllib2.urlopen(req)
    data = resp.read()
    # print data
    signal.alarm(0)
    return json.loads(data)

def sparkGetVars(dev, access_token, vars):
    signal.signal(signal.SIGALRM, alarmHandler)

    res = {}
    for v in vars:
         try:
             res[v] = None
             vals = sparkGetVar(dev, access_token, v)
             res[v] = vals['result']
         except urllib2.URLError as e:
             print "HTTP Exception:", str(e)
             print "Error body:", e.read()
         except Exception as e:
             print "**** Exception getting " + v
             traceback.print_exc()
    return res


# Main code ----------------------------------------------------

all_vars = [ 'upTime', 'connectTime', 'wifiRSSI', 
               'powerWatts', 'powerVA', 'mainsFreq', 
               'totalWh', 'sinPhi' ]

def toStr(val, fmt="%.1f"):
    if val is None:
        return 'None'
    else:
        return fmt % val

def getCSV( valList ):
    now = time.localtime()
    timedate = [ time.strftime("%d/%m/%y", now), time.strftime("%H:%M:%S",now) ]
    valStrs = [ toStr(v) for v in valList ]
    return ",".join( timedate + valStrs )
    
if __name__ == '__main__':
    from optparse import OptionParser
    parser = OptionParser("usage: %prog [options] device_name [variableName ...]")
    parser.add_option("-t", "--polltime", action="store", type="int", dest="poll_time", default=None,
        metavar="TIME", help="Repeatedly fetch data, pause TIME between fetches")
    parser.add_option("-c", "--csv", action="store_true", dest="csv", default=False,
        help="Output data in CSV format")

    (opts, posArgs) = parser.parse_args()
    
    if len(posArgs) < 1:
        parser.error("*** Please give a device name")
    dev = posArgs[0]
    if len(posArgs) == 1:
        vars = all_vars
    else:
        vars = posArgs[1:]
        
    access_token = getAccessToken()
    
    while True:
        vals = sparkGetVars(dev, access_token, vars)

        if opts.csv:
            print getCSV([ vals[v] for v in vars ])
            sys.stdout.flush()
        else:
            for v in vars:
                print "%-20s: %s" % (v, toStr(vals[v]))

        if opts.poll_time is None:
            break
        time.sleep(opts.poll_time)
