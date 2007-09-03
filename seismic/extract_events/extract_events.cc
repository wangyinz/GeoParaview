#include <vector>
#include <list>
#include <string>
#include <sstream>
#include "dbpp.h"
#include "filter++.h"
#include "SeisppKeywords.h"
#include "TimeSeries.h"
#include "ThreeComponentSeismogram.h"
#include "ensemble.h"
#include "Metadata.h"
#include "Hypocenter.h"
#include "resample.h"
#include "seispp.h"
#include "tr.h"
#include "msd.h"
using namespace std;
using namespace SEISPP;
/*! \brief Generic algorithm to load arrival times from a database.

In passive array processing a very common need is to extract time windows
around marked phase pick or a theoretical arrival time.  For measured
arrival times the css database has an awkward link to waveforms that 
causes problems when dealing with continuous data.  This is one element of
a set of functions aimed at dealing with this problem.  

This particular procedure aims to take an input ensemble of data and 
find matches for arrival times in an external database.  For each member
of the ensemble with a matching arrival it posts the arrival time to the
generalized header (Metadata) of the parent object.  To avoid testing 
for valid arrivals in the ensemble after this procedure is run a index
of data with valid arrivals is returned as an stl list container of ints.

\param dat is the input ensemble passed as a reference to the vector 
	container of data objects.  The type of the objects in the container
	is generic except class T MUST be an object that inherits Metadata.
	At time of this writing T could be TimeSeries, ThreeComponentSeismogram,
	or ComplexTimeSeries. 
\param dbh match handle to a Datascope database containing arrival. 
	Normally this should reference the standard catalog view.  That
	is:  event->origin->assoc->arrival subsetted to orid==prefor.
	The algorithm actually only cares that a find using the Metadata
	area of the vector data components will provide a unique match
	into the table the handle references (normally arrival).  This 
	tacitly assumes the handle has been properly constructed and the
	proper attributes have been loaded with the data to provide a unique
	match into arrival.  With css this REQUIRES that each component of
	dat MUST have had evid and/or orid posted before calling this function.  There
	is no other unambiguous way to match waveforms in this context to
	a unique arrival. 
\param keyword is the attribute name used to posted the arrival time data.

\return list of members of input vector with valid arrival times posted.
	The contents of the list can be traversed and used with operator[]
	to extract valid data from the ensemble.  
*/
template <class Tvec> 
	list<int> LoadArrivalTimes(vector<Tvec>& dat, 					DatascopeMatchHandle& dbh,
					const string keyword)
{
	int i;
	/* This construct rather than a simpler vector<T>::iterator
	* declaration seems necessary for gcc.  Not sure why. glp 8/2007*/
	typedef typename std::vector<Tvec> vector_type;
	typename vector_type::iterator d;
	list<int> data_with_arrivals;
	const string base_error("Warning (LoadArrivalTimes): ");
	for(d=dat.begin(),i=0;d!=dat.end();++d,++i)
	{
		double atime;  //  arrival time.  
		if(d->live)
		{
		// First see if there is an arrival for this
		// station.  If not, skip it. 
			list<int> records
				=dbh.find(dynamic_cast<Metadata&>(*d));
			// if no arrival silently skip data for this station
			if(records.size()<=0) continue;
			if(records.size()>1)
			{
				string sta=d->get_string("sta");
				cerr << base_error 
					<< "found "
					<< records.size()
					<< " arrivals for station "
					<< sta <<endl
					<< "Using first found "
					<< "in database view"<<endl;
			}
			Dbptr db=dbh.db;
			// tricky usage here.  begin() returns
			// an iterator so the * operator gets the
			// value = record number of the match
			db.record=*(records.begin());
			char csta[10];
			if(dbgetv(db,0,"arrival.time",&atime,"sta",csta,0)
				== dbINVALID) 
			{
				string sta=d->get_string("sta");
				cerr << base_error
					<< "dbgetv failed"
					<< " in attempt to obtain"
					<< " arrival time for station"
					<< sta << endl
					<< "Data from this station"
					<< " will be dropped"<<endl;	
			}
			d->put(keyword,atime);
			data_with_arrivals.push_back(i);
		}
	}
	return(data_with_arrivals);
} 
ThreeComponentEnsemble *BuildRegularGather(ThreeComponentEnsemble& raw,
	DatascopeMatchHandle& dbh, 
	ResamplingDefinitions& rdef,
	double target_dt,
	TimeWindow processing_window,
	bool use_arrival)
{
	string arrival_keyword("arrival.time");
	//fractional sample rate tolerance
	const double samprate_tolerance(0.01);  
	int nmembers=raw.member.size();
	auto_ptr<TimeSeries> x1,x2,x3;
	ThreeComponentEnsemble *result;
	result = new ThreeComponentEnsemble(raw);
	// An inefficiency here, but this allow us to discard dead
	// traces and problem data from ensemble as we assemble the
	// new one.
	result->member.clear();
	result->member.reserve(raw.member.size());
	// Load arrivals from database.  List returned is index into raw of
	// data with valid arrivals loaded
	list<int> data_with_arrivals;
	list<int>::iterator index;
	/* When using only arrival table entries this list is produced
	by the LoadArrivalTimes template by matching seismogram objects
	to arrivals found in the db.  Otherwise the list is just loaded
	with the sequence of integers from 1 to the number of entries 
	in the ensemble */
	if(use_arrival)
		data_with_arrivals=LoadArrivalTimes<ThreeComponentSeismogram>
					(raw.member,dbh,arrival_keyword);
	else
	{
		arrival_keyword=predicted_time_key;
		for(int i=0;i<raw.member.size();++i)
			data_with_arrivals.push_back(i);
	}
	ThreeComponentSeismogram d;
	for(index=data_with_arrivals.begin();index!=data_with_arrivals.end();++index)
	{
		d=raw.member[*index];
		if(d.live)
		{
		try {
			d.rotate_to_standard();	
			// partial clone used to hold result
			ThreeComponentSeismogram d3c(d);  
			x1=auto_ptr<TimeSeries>(ExtractComponent(d,0));
			x2=auto_ptr<TimeSeries>(ExtractComponent(d,1));
			x3=auto_ptr<TimeSeries>(ExtractComponent(d,2));
			// resample if necessary.  Using auto_ptr to avoid temporary pointer
			// and as good practice to avoid memory leaks
			if( (abs( (d.dt)-target_dt)/target_dt) > samprate_tolerance)
			{
				*x1=ResampleTimeSeries(*x1,rdef,target_dt,false);
				*x2=ResampleTimeSeries(*x2,rdef,target_dt,false);
				*x3=ResampleTimeSeries(*x3,rdef,target_dt,false);
			}
			// This procedure returns an auto_ptr.  An inconsistency in
			// SEISPP due to evolutionary development
			x1=ArrivalTimeReference(*x1,arrival_keyword,processing_window);
			x2=ArrivalTimeReference(*x2,arrival_keyword,processing_window);
			x3=ArrivalTimeReference(*x3,arrival_keyword,processing_window);
			double atime=x1->get_double(arrival_keyword);
			// safer than using attribute ns in x1
			// assumes all three components are equal length,
			// which is pretty much guaranteed here
			int ns=x1->s.size();
			if(ns>0)
			{
				d3c.ns=ns;
				d3c.dt=x1->dt;
				d3c.t0=x1->t0;
				d3c.tref=x1->tref;
				d3c.u=dmatrix(3,ns);
				// Convert to absolute time
				d3c.rtoa(atime);
				// Using blas here for speed
				dcopy(ns,&(x1->s[0]),1,d3c.u.get_address(0,0),3);
				dcopy(ns,&(x2->s[0]),1,d3c.u.get_address(1,0),3);
				dcopy(ns,&(x3->s[0]),1,d3c.u.get_address(2,0),3);
				result->member.push_back(d3c);
			}
		} catch (SeisppError serr)
		{
			// Minor maintenance issue here.  Frozen name is
			// assumed to be in Metadata area.  Avoiding second
			// handler for possible MetadataError intentionally
			string sta=d.get_string("sta");
			cerr << "Problem assembling 3C seismogram for station "
				<< sta <<endl;
			serr.log_error();
			cerr << "Data for this station dropped"<<endl;
		}
		}
			
	}
	return(result);
}
void PostEvid(ThreeComponentEnsemble *d,int evid)
{
	vector<ThreeComponentSeismogram>::iterator dptr;
	for(dptr=d->member.begin();dptr!=d->member.end();++dptr)
		dptr->put("evid",evid);
}
/*! \brief Builds a standard catalog view from a CSS3.0 database.

Much passive array processing is built around the css3.0 schema.
The css3.0 schema has a standard view that defines the definitive
catalog for a network.  This is formed by the join of:
	event->origin->assoc->arrival
and is usually (as here) subsetted to only keep rows with
orid equal to the "preferred origin" (prefor).  This procedure
builds a handle to this database view.

\param dbh is a handle to the parent Datascope database 
	from which the view is to be constructed.  It need
	only reference the correct database. 
\return new handle that refers to the "standard" catalog view
	described above.
*/
DatascopeHandle StandardCatalogView(DatascopeHandle& dbh)
{
	DatascopeHandle result(dbh);
	dbh.lookup("event");
	dbh.natural_join("origin");
	string ss_to_prefor("orid==prefor");
	dbh.subset(ss_to_prefor);
	dbh.natural_join("assoc");
	dbh.natural_join("arrival");
	return(dbh);
}
void ApplyFST(ThreeComponentEnsemble& e,Hypocenter& hypo,
	double vp0,double vs0)
{
        for(int i=0;i<e.member.size();++i)
        {
                double lat,lon,elev;
                lat=e.member[i].get_double("lat");
                lon=e.member[i].get_double("lon");
                elev=e.member[i].get_double("elev");
                SlownessVector u=hypo.pslow(lat,lon,elev);
                e.member[i].free_surface_transformation(u,vp0,vs0);
        }
}
/* These two routines build dir and dfile names respectively.  Currently
the structure is frozen.  Ultimately needs a more general approach 
ala antelope's trace library. */
string BuildDirName(ThreeComponentEnsemble *g,string base)
{
	/* for now we always uses evid as a dir name */
	string result;
	char buf[12];
	stringstream ss(buf);
	ss << g->get_int("evid");
	result=base + "/" + ss.str();
	return(result);
}
string BuildDfileName(ThreeComponentEnsemble *g,ThreeComponentSeismogram& member, string chan)
{
	string result;
	char buf[32];
	stringstream ss(buf);
	// This assumes we can grab these from the ensemble metadata.
	// they are set in main in this program.  Beware if copied
	// Intentionally do not trap errors as we can assume these
	// are set here.
	string year=g->get_string("year");
	string jday=g->get_string("jday");
	int evid=g->get_int("evid");
	string sta=member.get_string("sta");
	ss << sta<<"_" 
		<<year << "_"
		<<jday<<"_"
		<<evid<<"."
		<<chan;
	return(ss.str());
}
string getfullpath(string dir)
{
	int ierr;
	char buf[FILENAME_MAX];
	ierr=abspath(const_cast<char *>(dir.c_str()),buf);
	return(string(buf));
}
/* This procedure standardizes an error message for the save routine immediately
below this */
void SaveWarning(string sta, string chan, int evid, double t0, string mes)
{
	if(SEISPP::SEISPP_verbose)
	{
		char *stime=strtime(t0);
		cerr << "SaveResults(Warning):  "
			<< "station "<<sta
			<<", channel "<<chan
			<<", evid "<<evid
			<<" at time="<<stime
			<< endl
			<< mes <<endl;
		free(stime);
	}
}
/* This routine assumes output is wfdisc and uses the simple trputwf
routine to allow variable datatype.*/

void SaveResults(DatascopeHandle& dbh,
	ThreeComponentEnsemble *gather,
		AttributeMap& amo,
			string chans[3],
				string datatype,
					string dir)
{
	int i,ns;
	/* These are used only when miniseed is the output format */
	int *idptr=0;
	int nints=0;
	int nbytes=0;
	Wftype *wftype=trwftype((char *)"sd");
	// This is special for this program.  Normally couldn't expect
	// this to be available.
	int evid=gather->get_int("evid");
	vector<ThreeComponentSeismogram>::iterator d;
	for(d=gather->member.begin();d!=gather->member.end();++d)
	{
	    if((d->live) && !(d->has_gap()))
	    {
		try {
			// These must be in the trace metadata or data 
			// will be dropped
			string sta=d->get_string("sta");
			string net("XX");
			if(datatype=="sd")
				net=d->get_string("net");
			double time=d->t0;
			double endtime=d->endtime();
			// Must assume calib is equal for all components
			double calib=d->get_double(gain_keyword);
			double samprate=1.0/(d->dt);
			int k;
			for(k=0;k<3;++k)
			{
				TimeSeries *x;
				x=ExtractComponent(*d,k);
		
				ns=d->ns;
				string dfile=BuildDfileName(gather,*d,chans[k]);
				/* NOTE the following two routines need an error
				return trap */
				dbh.db.record=dbaddv(dbh.db,0,
					"sta",sta.c_str(),
					"chan",chans[k].c_str(),
					"time",time,
					"endtime",endtime,
					"jdate",yearday(time),
					"nsamp",ns,
					"samprate",samprate,
					"calib",calib,
					"dir",dir.c_str(),
					"dfile",dfile.c_str(),
					"datatype",datatype.c_str(),
					0);
				if(dbh.db.record<0)
				{
					string mes("dbaddv error on wfdisc.  ");
					mes=mes+string("Data not saved for this entry in the db.\n");
					mes=mes+string("You may need to edit output wfdisc.");
					SaveWarning(sta,chans[k],evid,time,mes);
				}
				else if(datatype=="sd")
				{
					/* this code is based on 4.9 example/c/tr/db2miniseed.c */
					Msd *msd;
					msd=msdnew();
					/* This is the ugly seed net/sta/chan/loc mapping problem.
					Here we assume data are from a css db so sta and net are
					distinct.  Chan, however, can contain a loc code so we 
					call the routine to split the chan field */
					char fchan[16],loc[16];
					seed_loc(const_cast<char *>(sta.c_str()),
						const_cast<char *>(chans[k].c_str()),
						fchan,loc);
					/* Default MSD_LEVEL and MSD_RECORD_SIZE define as option in 
					the example.  These are items set in the example program. */
					string absdir=getfullpath(dir);
					makedir(const_cast<char *>(absdir.c_str()));
					string path=absdir+"/"+dfile;
					
					msdput(msd,MSD_FILENAME,path.c_str(),
						MSD_NET, net.c_str(),
						MSD_STA,sta.c_str(),
						MSD_CHAN,fchan,
						MSD_LOC,loc,
						MSD_TIME,time,
						MSD_SAMPRATE,samprate,
						0 );
					free(fchan);  free(loc);
					int problem,retcode;
					int *itrd=new int[ns];
					for(i=0;i<ns;++i)
						itrd[i]=static_cast<int>
						          ((x->s[i])/calib);
					problem=itr2ext(itrd,ns,wftype,(void **)(&idptr),
						&nints,&nbytes);
					if(problem & TR_TRUNCATED)
					{
						string mes("tr2ext truncated data");
						SaveWarning(sta,chans[k],evid,time,mes);
					}
					if(problem & TR_CLIPPED)
					{
						string mes("tr2ext detected clipped data");
						SaveWarning(sta,chans[k],evid,time,mes);
					}
					// save_record is defaulted with 0 as arg 2
					retcode=cmsd(msd,0,idptr,ns);
					retcode+=cmsd(msd,0,idptr,0);
					msdclose(msd);
					msdfree(msd);
					delete [] itrd;
				}
				else
				{
					float *trd=new float[ns];
					for(i=0;i<ns;++i) 
						trd[i]=static_cast<float>
						  ((x->s[i])/calib);
					if(trputwf(dbh.db,trd))
					{
					  string mes("trputwf failed");
					  SaveWarning(sta,chans[k],evid,time,mes);
					}
					delete [] trd;
				}
				delete x;
			}
		} catch (SeisppError serr) {
			string sta=d->get_string("sta");
			cerr << "Error saving station "<<sta<<endl;
			serr.log_error();
		}
	    }
	}
	if(idptr!=NULL) free(idptr);
	free(wftype);
}
/* Ugly approach to a problem that needs a general solution.  seispp
uses some convenient short names that need mapping into full names that
include a table specifier.  Here we do this crudly with a directly list
of required attributes I know will need to be set */
void map_to_wfprocess(ThreeComponentSeismogram& d)
{
	/* A try block is avoided on purpose here because these names are all internal
	and should always be defined */
	int ival;
	ival=d.get_int("nsamp");
	d.put("wfprocess.nsamp",ival);
	double dval;
	dval=d.get_double("time");
	d.put("wfprocess.time",dval);
	dval=d.get_double("endtime");
	d.put("wfprocess.endtime",dval);
	dval=d.get_double("samprate");
	d.put("wfprocess.samprate",dval);
}
void usage()
{
	cerr << "extract_events dbin dbout [-e eventdb -s eventsubset -pf pfname]" << endl;
	exit(-1);
}
bool SEISPP::SEISPP_verbose=false;
int main(int argc, char **argv)
{
	// This is a C++ thing.  Not required on all platforms, but
	// recommended by all the books.  
	ios::sync_with_stdio();
	// This is a standard C construct to crack the command line
	if(argc<3) usage();
	string pfin("extract_events");
	string dbin(argv[1]);
	string dbout(argv[2]);
	string eventdb=dbin;
	bool eventsubset=false;
	string event_subset_expression("NONE");
	int i;
        for(i=3;i<argc;++i)
        {
                if(!strcmp(argv[i],"-V"))
                        usage();
                else if(!strcmp(argv[i],"-pf"))
                {
                        ++i;
                        if(i>=argc) usage();
                        pfin = string(argv[i]);
                }
		else if(!strcmp(argv[i],"-e"))
		{
                        ++i;
                        if(i>=argc) usage();
			eventdb=string(argv[i]);
		}
		else if(!strcmp(argv[i],"-s"))
		{
                        ++i;
                        if(i>=argc) usage();
			eventsubset=true;
			event_subset_expression=string(argv[i]);
		}
		else if(!strcmp(argv[i],"-v"))
			SEISPP::SEISPP_verbose=true;
                else
                        usage();
        }
	// Standard Antelope C construct to read a parameter file
	// Well almost standard.  const_cast is a C++ idiom required
	// due to a collision of constantness with Antelope libraries.
	Pf *pf;
        if(pfread(const_cast<char *>(pfin.c_str()),&pf)) 
	{
		cerr << "pfread error for pf file="<<pfin<<".pf"<<endl;
		exit(-1);
	}
	Metadata control(pf);
	MetadataList mdens=pfget_mdlist(pf,"Ensemble_mdlist");
	MetadataList mdtrace=pfget_mdlist(pf,"station_mdlist");
	MetadataList mdlo=pfget_mdlist(pf,"output_mdlist");
	try {
		// Get set of control parameters
		string netname=control.get_string("netname");
		string phase=control.get_string("phase");
		double ts,te;
		ts=control.get_double("data_window_start");
		te=control.get_double("data_window_end");
		TimeWindow datatwin(ts,te);
		double tpad=control.get_double("data_time_pad");
		ts=control.get_double("processing_window_start");
		te=control.get_double("processing_window_end");
		TimeWindow processing_twin(ts,te);
		StationChannelMap stachanmap(pf);
		string schemain=control.get_string("InputAttributeMap");
		string schemaout=control.get_string("OutputAttributeMap");
		double target_dt=control.get_double("target_sample_interval");
		string method=control.get_string("method");
		string model=control.get_string("model");
		// When true load only data with marked times of requested
		// phase in the arrival table.  Otherwise use predicted
		// time
		bool use_arrival=control.get_bool("use_arrival_table");
		/* When true data are stored in wfprocess as 3c data objects.
		Otherwise data are saved in more conventional wfdisc */
		bool save_as_3c=control.get_bool("save_as_3c_objects");
		bool require_event=control.get_bool("require_event");
		string outchans[3];
		outchans[0]=control.get_string("X1_channel_name");
		outchans[1]=control.get_string("X2_channel_name");
		outchans[2]=control.get_string("X3_channel_name");
		bool apply_fst=control.get_bool("apply_free_surface_transformation");
		if(apply_fst && save_as_3c)
		{
			cerr << "Illegal parameter selection"<<endl
				<< "Free surface transformation data cannot be saved"
				<< " as 3c objects" <<endl
				<< "Edit pf file and try again"<<endl;
			exit(-1);
		}
		double vp0,vs0;
		vp0=control.get_double("vp0");
		vs0=control.get_double("vs0");
		string datatype=control.get_string("datatype");
		/* For now this is a fixed location.  Eventually needs to be
		similar to antelope approach allowing variable directory
		naming.*/
		string basedir=control.get_string("output_waveform_directory_base");
		string filter_param=control.get_string("filter");
		TimeInvariantFilter filt(filter_param);
		ResamplingDefinitions rdef(pf);
		// First get all the database components assembled.
		// There are three here:  input data, output data, and beam data
		DatascopeHandle dbh(dbin,true);
		DatascopeHandle dbho(dbout,false);
		/* Build a match handle for arrivals using the standard catalog
		view of the join of event->origin->assoc->arrival */
		AttributeMap am(schemain);  
		AttributeMap amo(schemaout);  
		DatascopeHandle dbcatalog(dbh);
		if(eventdb!=dbin)
			dbcatalog=DatascopeHandle(eventdb,false);
		if(use_arrival)
			dbcatalog=StandardCatalogView(dbcatalog);
		else if(require_event)
		{
			dbcatalog.lookup(string("event"));
			dbcatalog.natural_join(string("origin"));
			dbcatalog.subset(string("orid==prefor"));
		}
		else
			dbcatalog.lookup(string("origin"));
		if(event_subset_expression!="NONE")
		{
			dbcatalog.subset(event_subset_expression);
			if(dbcatalog.number_tuples()<=0)
			{
				cerr << "Event subset expression= "
					<< event_subset_expression
					<< " yielded no data.  Exiting."
					<< endl;
			}
		}
		list<string> matchkeys;
		matchkeys.push_back(string("sta"));
		matchkeys.push_back(string("evid"));
		DatascopeMatchHandle dbhm(dbcatalog,string(""),matchkeys,am);
		if(save_as_3c)
			dbho.lookup(string("wfprocess"));
		else
			dbho.lookup(string("wfdisc"));
		// These are pointers to dynamic objects used in the loop
		// below.  
		ThreeComponentEnsemble *rawdata=NULL, *regular_gather=NULL,
				*decondata=NULL;
		SeismicArray *stations=NULL;
		/* Assorted variables needed in the loop below */
		double lat,lon,depth,otime;
		int evid;
		int record;
		string dir,dfile;
		char *year, *jday;
		dbcatalog.rewind();
		for(record=0;record<dbcatalog.number_tuples();++record,++dbcatalog)
		{
			lat=dbcatalog.get_double("lat");
			lon=dbcatalog.get_double("lon");
			lat=rad(lat);
			lon=rad(lon);
			depth=dbcatalog.get_double("depth");
			otime=dbcatalog.get_double("time");
			if(require_event)
				evid=dbcatalog.get_int("evid");
			else
				evid=dbcatalog.get_int("orid");
			Hypocenter hypo(lat,lon,depth,otime,
				method,model);

			// On the first record we need to load the station
			// geometry object
			if(record==0)
			{
				stations = new SeismicArray(
					dynamic_cast<DatabaseHandle&>(dbh),
					hypo.time,netname);
			}
			else
			{
				TimeWindow test(hypo.time,hypo.time+2000.0);
				if(!stations->GeometryIsValid(test))
				{
					delete stations;
					stations = new SeismicArray(
					dynamic_cast<DatabaseHandle&>(dbh),
					hypo.time,netname);
				}
			}
			// Read the raw data using the time window based constructor
			rawdata=array_get_data(*stations,hypo,
				phase,datatwin,tpad,dynamic_cast<DatabaseHandle&>(dbh),
				stachanmap,mdens,mdtrace,am);
			PostEvid(rawdata,evid);
			if(!use_arrival)
			{
				StationTime predtimes=ArrayPredictedArrivals(*stations,hypo,phase);
				LoadPredictedTimes(*rawdata,predtimes,predicted_time_key,phase);
			}
			FilterEnsemble(*rawdata,filt);

			regular_gather=BuildRegularGather(*rawdata, dbhm,rdef,target_dt,
					processing_twin,use_arrival);
			delete rawdata;
			if(apply_fst)
				ApplyFST(*regular_gather,hypo,vp0,vs0);
			/* Now save data.  */
			year=epoch2str(otime,(char *)"%Y");
			jday=epoch2str(otime,(char *)"%j");
			/* These are used to build directory and file names */
			regular_gather->put("evid",evid);
			regular_gather->put("year",year);
			regular_gather->put("jday",jday);
			free(year);
			free(jday);
			dir=BuildDirName(regular_gather,basedir);
			if(save_as_3c)
			{
				for(i=0;i<regular_gather->member.size();++i)
				{
					int rec;
					regular_gather->member[i].put("wfprocess.dir",dir);
					// set chan field for dfile 
					regular_gather->put("chan",(char *)"3C");
					dfile=BuildDfileName(regular_gather,
						regular_gather->member[i],
						string("3C"));
					regular_gather->member[i].put("wfprocess.dfile",dfile);
					regular_gather->member[i].put("wfprocess.datatype","3c");
					regular_gather->member[i].put("wfprocess.timetype","a");
					/* A more elegant solution is needed for the following
					It is necesary because we've used short names for certain
					attributes that need to be expanded for wfprocess.*/
					map_to_wfprocess(regular_gather->member[i]);
					rec=dbsave(regular_gather->member[i],
						dbho.db,
						string("wfprocess"),
						mdlo, amo);
				}
			}
			else
			{
				SaveResults(dbho,regular_gather,
					am,outchans,datatype,dir);
			}
			
			delete regular_gather;
		}
	}
	catch (SeisppError serr)
	{
		serr.log_error();
	}
	catch (MetadataGetError mdge)
	{
		mdge.log_error();
	}
	catch (MetadataError mde)
	{
		mde.log_error();
	}
	catch (...)
	{
		cerr << "Something threw an unexpected exception"<<endl;
	}
}

