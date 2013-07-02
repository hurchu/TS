# Copyright (C) 2012 Ion Torrent Systems, Inc. All Rights Reserved
from django import http
from datetime import datetime
from django.contrib.auth.decorators import login_required
from django.template import RequestContext
from django.shortcuts import render_to_response, get_object_or_404, \
    get_list_or_404
from django.conf import settings
from django.db import transaction
from django.http import HttpResponse

from iondb.rundb.models import PlannedExperiment, RunType, ApplProduct, \
    ReferenceGenome, Content, KitInfo, VariantFrequencies, dnaBarcode, \
    LibraryKey, ThreePrimeadapter, Chip, QCType, Project, Plugin, \
    PlannedExperimentQC, Sample

from traceback import format_exc
import json
import simplejson
import uuid

import logging
from django.core import serializers
from iondb.rundb.api import PlannedExperimentResource, RunTypeResource, \
    dnaBarcodeResource, ChipResource

from django.core.urlresolvers import reverse

from iondb.utils import toBoolean
from iondb.rundb.plan.views_helper import get_projects, dict_bed_hotspot, \
    is_valid_chars, is_invalid_leading_chars, is_valid_length

from iondb.rundb.plan.plan_csv_writer import get_template_data_for_batch_planning
from iondb.rundb.plan.plan_csv_validator import validate_csv_plan

import os
import string
import traceback
import tempfile
import csv

from django.core.exceptions import ValidationError

logger = logging.getLogger(__name__)

MAX_LENGTH_PLAN_NAME = 512
MAX_LENGTH_SAMPLE_NAME = 127
MAX_LENGTH_PROJECT_NAME = 64
MAX_LENGTH_NOTES = 1024

ERROR_MSG_INVALID_CHARS = " should contain only numbers, letters, spaces, and the following: . - _"
ERROR_MSG_INVALID_LENGTH = " length should be %s characters maximum. "
ERROR_MSG_INVALID_LEADING_CHARS = " should only start with numbers or letters. "


@login_required
def plans(request):
    """
    plan template home page
    """
    ctxd = {}

    ctx = RequestContext(request, ctxd)
    return render_to_response("rundb/plan/plans.html", context_instance=ctx, mimetype="text/html")


@login_required
def planned(request):
    ctx = RequestContext(request)
    return render_to_response("rundb/plan/planned.html", context_instance=ctx)


@login_required
def delete_plan_template(request, pks=None):
    #TODO: See about pulling this out into a common methods
    pks = pks.split(',')
    _type = 'plannedexperiment'
    planTemplates = get_list_or_404(PlannedExperiment, pk__in=pks)
    _typeDescription = "Template" if planTemplates[0].isReusable is True else "Planned Run"
    actions = []
    for pk in pks:
        actions.append(reverse('api_dispatch_detail', kwargs={'resource_name': _type, 'api_name': 'v1', 'pk': int(pk)}))
    names = ', '.join([x.planName for x in planTemplates])
    ctx = RequestContext(request, {
        "id": pks[0], "name": names, "ids": json.dumps(pks), "names": names, "method": "DELETE", 'methodDescription': 'Delete', "readonly": False, 'type': _typeDescription, 'action': actions[0], 'actions': json.dumps(actions)
    })
    return render_to_response("rundb/common/modal_confirm_delete.html", context_instance=ctx)


@login_required
def add_plan_template(request, code):

    """prepare data to guide user in plan template creation"""
    return _add_plan(request, code, "New")


@login_required
def add_plan_no_template(request, code):
    """
    Create a planned run *without* a template via wizard
    """
    return _add_plan(request, code, "Plan Run New")


##def  _handle_request_preProcess_failure():
##    result = HttpResponse("error message here", content_type="text/plain")
##    result.status_code = 417
##    return result


def _add_plan(request, code, intent):
    """prepare data to guide user in plan template creation"""

    #logger.debug("TIMING START - _add_plan for either plan or template...");

    isForTemplate = True
    if (intent == "Plan Run New"):
        isForTemplate = False
        
    data = _get_allApplProduct_data(isForTemplate)
    codes = {
        '1': "AMPS",
        '2': "TARS",
        '3': "WGNM",
        '4': "RNA",
        '5': "AMPS_RNA",
        '6': "AMPS_EXOME"
    }
    product_code = codes.get(code, "GENS")
    logger.debug("views.add_planTemplate()... code=%s, product_code=%s" % (code, product_code))

    ctxd = {
        "intent": intent,
        "planTemplateData": data,
        "selectedPlanTemplate": None,
        "selectedApplProductData": data[product_code]
    }

    context = RequestContext(request, ctxd)

    return render_to_response("rundb/plan/modal_plan_wizard.html", context_instance=context)


def _get_runtype_json(request, runType):
    rtr = RunTypeResource()
    rt = rtr.obj_get(runType=runType)
    rtr_bundle = rtr.build_bundle(obj=rt, request=request)
    rt_json = rtr.serialize(None, rtr.full_dehydrate(rtr_bundle), 'application/json')
    return rt_json


def _get_dnabarcode_json(request, barcodeId):
    dnar = dnaBarcodeResource()
    dna = dnar.obj_get_list(name=barcodeId, request=request).order_by('index')
    dna_bundles = [dnar.build_bundle(obj=x, request=request) for x in dna]
    dna_json = dnar.serialize(None, [dnar.full_dehydrate(bundle) for bundle in dna_bundles], 'application/json')
    return dna_json


def _get_chiptype_json(request, chipType, plan):
    chipResource = ChipResource()
    chipResource_serialize_json = None
    if chipType:
        try:
            try:
                chip = chipResource.obj_get(name = chipType)
            except Chip.DoesNotExist:
                chip = chipResource.obj_get(name = chipType[:3])

            chipResource_bundle = chipResource.build_bundle(obj=chip, request=request)
            chipResource_serialize_json = chipResource.serialize(None, chipResource.full_dehydrate(chipResource_bundle), 'application/json')
        except Chip.DoesNotExist:
            logger.error("views._get_chiptype_json() Plan.pk=%d; name=%s has invalid chip type=%s" %(plan.id, plan.planName, chipType))
            chipResource_bundle = None
            chipResource_serialize_json = json.dumps("INVALID")
            
    else:
        chipResource_bundle = None
        chipResource_serialize_json = json.dumps(None)
    return chipResource_serialize_json


def _get_stripped_chipType(chipType):
    ''' 
    some historical runs are found to have chipType is extra double quotes and/or backslash with quotes
    0047_ migration script should have fixed the extra double quotes problems. For defensive coding, double check here
    '''
    if chipType:
        return chipType.replace('\\', '').replace('"', '')
    else:
        return chipType

    
def _review_plan(request, pk):
    per = PlannedExperimentResource()
    pe = per.obj_get(pk=pk)
    per_bundle = per.build_bundle(obj=pe, request=request)
    pe_json = per.serialize(None, per.full_dehydrate(per_bundle), 'application/json')

    rt_json = _get_runtype_json(request, pe.runType)
    dna_json = _get_dnabarcode_json(request, per_bundle.data.get('barcodeId'))
    chipType_json = _get_chiptype_json(request, _get_stripped_chipType(per_bundle.data.get('chipType')), pe)

    return render_to_response("rundb/plan/modal_review_plan.html", {
                              "plan": pe,
                              "selectedPlanTemplate": pe_json,
                              "selectedRunType": rt_json,
                              "selectedBarcodes": dna_json,
                              "view": 'template' if 'template' in request.path else 'Planned Run',
                              "selectedChip": chipType_json
                              })


@login_required
def review_plan_template(request, _id):
    """
    Review plan template contents
    """
    return _review_plan(request, _id)


@login_required
def review_plan_run(request, _id):
    """
    Review plan contents
    """
    return _review_plan(request, _id)


@login_required
def edit_plan_template(request, template_id):
    """
    Edit plan template in template wizard
    """

    context = _plan_template_helper(request, template_id, True, "Edit")
    #logger.debug("TIMING create_plan_from_template B4 if planplugins in planTemplate.selectedPlugins.keys()...");
    return render_to_response("rundb/plan/modal_plan_wizard.html", context_instance=context)


@login_required
def edit_plan_run(request, _id):
    """
    Edit plan in template wizard
    """

    context = _plan_template_helper(request, _id, False, "EditPlan")

    return render_to_response("rundb/plan/modal_plan_wizard.html", context_instance=context)


@login_required
def copy_plan_run(request, _id):
    """
    Copy plan in template wizard
    """
        
    context = _plan_template_helper(request, _id, False, "CopyPlan")

    return render_to_response("rundb/plan/modal_plan_wizard.html", context_instance=context)


def _plan_template_helper(request, _id, isForTemplate, intent):
    data = _get_allApplProduct_data(isForTemplate)
    planTemplate = get_object_or_404(PlannedExperiment, pk=_id)
    runType = get_object_or_404(RunType, runType=planTemplate.runType)

    # add experiment attributes
    experiment = planTemplate.experiment
    exp_keys = [
          ('autoAnalyze', 'autoAnalyze'),
          ('flows', 'flows'),
          ('notes', 'notes'),
          ('sequencekitname', 'sequencekitname'),
    ]
    for plankey, key in exp_keys:
        setattr(planTemplate, plankey, getattr(experiment,key,''))

    chipResource = ChipResource()
    try:
        chip = chipResource.obj_get(name = experiment.chipType)
        if chip:
            setattr(planTemplate, "chipType", experiment.chipType)
    except Chip.DoesNotExist:
        try:
            chip = chipResource.obj_get(name = experiment.chipType[:3])
            if chip:
                setattr(planTemplate, "chipType", experiment.chipType[:3])
        except Chip.DoesNotExist:
            setattr(planTemplate, "chipType", experiment.chipType)
    
    setattr(planTemplate, "sample", experiment.get_sample())
    setattr(planTemplate, "sampleDisplayedName", experiment.get_sampleDisplayedName())
    
    setattr(planTemplate, "isIonChef", planTemplate.is_ionChef())
        
    # add EAS attributes
    if experiment:
        eas = experiment.get_EAS()
    else:
        eas = None
        
    eas_keys = [
          ('barcodedSamples', 'barcodedSamples'),
          ('barcodeId', 'barcodeKitName'),
          ('bedfile', 'targetRegionBedFile'),
          ('forward3primeadapter', 'threePrimeAdapter'),          
          ('libraryKey', 'libraryKey'),
          ('librarykitname', 'libraryKitName'),
          ('regionfile', 'hotSpotRegionBedFile'),
          ('selectedPlugins', 'selectedPlugins')
    ]
    for plankey, key in eas_keys:
        setattr(planTemplate, plankey, getattr(eas,key,''))

    if eas:
        setattr(planTemplate, "library", eas.reference if eas.reference != "none" else '')
    else:
        setattr(planTemplate, "library", "")
    
    chipTypeDetails = None
    if planTemplate.chipType:
        chipTypeDetails = get_object_or_404(Chip, name=planTemplate.chipType)

    selectedProjectNames = [selectedProject.name for selectedProject in list(planTemplate.projects.all())]
    logger.debug("views._plan_template_helper selectedProjectNames=%s" % selectedProjectNames)

    # mark plugins selected if any
    for plugin in data['plugins']:
        if plugin.name in planTemplate.selectedPlugins.keys():
            plugin.selected = True
            plugin.userInput = json.dumps(planTemplate.selectedPlugins[plugin.name].get('userInput',None))
        else:
            plugin.selected = False

    # mark uploaders selected if any
    for plugin in data['uploaders']:
        if plugin.name in planTemplate.selectedPlugins.keys():
            plugin.selected = True
         
            ##in case we're copying old plans that have dict {} data type for userInput. new format is list of dict [{}]
            userInput = planTemplate.selectedPlugins[plugin.name].get('userInput',None)
            
            plugin.userInput = json.dumps(userInput)
            
            if (isinstance(userInput, dict)):
                plugin.userInput = json.dumps([userInput])                
                    
            if 'IonReporter' in plugin.name:
                data['irConfigSaved'] = plugin.userInput
                data['irConfigSaved_version'] = 1.0 if plugin.name == 'IonReporterUploader_V1_0' else plugin.version           
         
        else:
            plugin.selected = False

    #planTemplateData contains what are available for selection
    #and what each application product's characteristics and default selection
    ctxd = {
        "intent": intent,
        "planTemplateData": data,
        "selectedApplProductData": "",
        "selectedPlanTemplate": planTemplate,
        "selectedRunType": runType,
        "selectedProjectNames": selectedProjectNames,
        "selectedChipTypeDetails": chipTypeDetails
    }
    context = RequestContext(request, ctxd)
    return context


@login_required
def copy_plan_template(request, template_id):
    """
    Clone plan template in template wizard
    """
    context = _plan_template_helper(request, template_id, True, "Copy")
    return render_to_response("rundb/plan/modal_plan_wizard.html", context_instance=context)


@login_required
def create_plan_from_template(request, template_id):
    """
    Create a plan run from existing template via wizard
    """
    #logger.debug("TIMING START - create_plan_from_template...");

    context = _plan_template_helper(request, template_id, False, "Plan Run")
    #logger.debug("TIMING create_plan_from_template B4 if planplugins in planTemplate.selectedPlugins.keys()...");
    return render_to_response("rundb/plan/modal_plan_wizard.html", context_instance=context)

@login_required
def batch_plans_from_template(request, template_id):
    """
    To create multiple plans from an existing template    
    """
        
    planTemplate = get_object_or_404(PlannedExperiment, pk=template_id)

    #planTemplateData contains what are available for selection
    ctxd = {
        "selectedPlanTemplate": planTemplate
    }
    context = RequestContext(request, ctxd)
    return render_to_response("rundb/plan/modal_batch_planning.html", context_instance=context)

@login_required
def getCSV_for_batch_planning(request, templateId, count):
    """
    To create csv file for batch planning based on an existing template    
    """
    
    #logger.debug("ENTER views.getCSV_for_batch_planning() templateId=%s; count=%s;" %(templateId, count))
    
    response = http.HttpResponse(mimetype='text/csv')
    now = str(datetime.now().strftime("%Y_%m_%d_%H_%M_%S"))
    response['Content-Disposition'] = 'attachment; filename=batchPlanning_%s.csv' % now
    
    hdr, body = get_template_data_for_batch_planning(templateId)
    
    writer = csv.writer(response)
    writer.writerow(hdr)
    
    index = 0
    max = int(count)
    while (index < max):
        writer.writerow(body)
        index += 1

    return response

@login_required
def upload_plans_for_template(request):
    """
    Allow user to upload a csv file to create plans based on a previously selected template
    """

    ctxd = {}

    context = RequestContext(request, ctxd)
    return render_to_response("rundb/plan/modal_batch_planning_upload.html", context_instance=context)


@login_required
@transaction.commit_manually
def save_uploaded_plans_for_template(request):
    """add plans, with CSV validation"""
    logger.info(request)
                            
    if request.method != 'POST':
        logger.exception(format_exc())
        transaction.rollback()
        return HttpResponse(json.dumps({"error": "Error, unsupported HTTP Request method (%s) for saving plan upload." % request.method}), mimetype="application/json")
               
    postedfile = request.FILES['postedfile']
    destination = tempfile.NamedTemporaryFile(delete=False)

    for chunk in postedfile.chunks():
        destination.write(chunk)
    postedfile.close()
    destination.close()

    #check to ensure it is not empty
    headerCheck = open(destination.name, "rU")
    firstCSV = []
    for firstRow in csv.reader(headerCheck):
        firstCSV.append(firstRow)            
        #logger.info("views.save_uploaded_plans_for_template() firstRow=%s;" %(firstRow))
        
    headerCheck.close()
    if not firstRow:
        os.unlink(destination.name)
        transaction.rollback()
        return HttpResponse(json.dumps({"status": "Error: batch planning file is empty"}), mimetype="text/html")
        
    index = 0
    plans = []
    rawPlanDataList = []
    failed = {}
    file = open(destination.name, "rU")
    reader = csv.DictReader(file)
    for index, row in enumerate(reader, start=1):
        errorMsg, aPlanDict, rawPlanDict, isToSkipRow = validate_csv_plan(row)
        
        logger.info("views.save_uploaded_plans_for_template() index=%d; errorMsg=%s; planDict=%s" %(index, errorMsg, rawPlanDict))
        if errorMsg:
            logger.info("views.save_uploaded_plans_for_template() ERROR MESSAGE index=%d; errorMsg=%s; planDict=%s" %(index, errorMsg, rawPlanDict))

            failed[index] = errorMsg
            continue
        elif isToSkipRow:
            logger.info("views.save_uploaded_plans_for_template() SKIPPED ROW index=%d; row=%s" %(index, row))            
            continue
        else:
            plans.append(aPlanDict)
            rawPlanDataList.append(rawPlanDict)

    destination.close()  # now close and remove the temp file
    os.unlink(destination.name)
    if index == 0:
        transaction.rollback()
        return HttpResponse(json.dumps({"status": "Error: There must be at least one plan! Please reload the page and try again with more plans."}), mimetype="text/html")

    if failed:
        r = {"status": "Plan validation failed. The plans have not been saved.", "failed": failed}
        logger.info("views.save_uploaded_plans_for_template() failed=%s" %(r))
       
        transaction.rollback()
        return HttpResponse(json.dumps(r), mimetype="text/html")

    #saving to db needs to be the last thing to happen
    try:
        index = 0
        for planFamily in plans:
            plan = planFamily['plan']
            plan.save()
                    
            expObj = planFamily['exp']
            expObj.plan = plan
            expObj.expName = plan.planGUID
            expObj.unique = plan.planGUID
            expObj.displayname = plan.planGUID
            expObj.save()

            easObj = planFamily['eas']
            easObj.experiment = expObj
            easObj.isEditable = True
            easObj.save()
            
            #saving/associating samples    
            sampleDisplayedNames = planFamily['samples']
            sampleNames = [name.replace(' ', '_') for name in sampleDisplayedNames]
            externalId = None
            for name, displayedName in zip(sampleNames,  sampleDisplayedNames): 
                sample_kwargs = {
                                'name' : name,
                                'displayedName' : displayedName,
                                'date' : plan.date,
                                'status' : plan.planStatus,
                                'externalId': externalId
                                }
    
                sample = Sample.objects.get_or_create(name=name, externalId=externalId, defaults=sample_kwargs)[0]
                sample.experiments.add(expObj)
                sample.save()
                        
            planDict = rawPlanDataList[index]
            
            # add QCtype thresholds
            qcTypes = QCType.objects.all()
            for qcType in qcTypes:
                qc_threshold = planDict.get(qcType.qcName, '')
                if qc_threshold:
                    # get existing PlannedExperimentQC if any
                    plannedExpQcs = PlannedExperimentQC.objects.filter(plannedExperiment=plan.id, qcType=qcType.id)
                    if len(plannedExpQcs) > 0:
                        for plannedExpQc in plannedExpQcs:
                            plannedExpQc.threshold = qc_threshold
                            plannedExpQc.save()
                    else:
                        kwargs = {
                            'plannedExperiment': plan,
                            'qcType': qcType,
                            'threshold': qc_threshold
                        }
                        plannedExpQc = PlannedExperimentQC(**kwargs)
                        plannedExpQc.save()

            # add projects
            projectObjList = get_projects(request.user, planDict)
            for project in projectObjList:
                if project:
                    plan.projects.add(project)

             
            index += 1
    except:
        logger.exception(format_exc())
        transaction.rollback()
        return HttpResponse(json.dumps({"status": "Error saving plans to database!"}), mimetype="text/html")
        ##return HttpResponse(json.dumps({"error": "Internal error while trying to save the plan."}), mimetype="application/json")
    else:
        transaction.commit()            
        r = {"status": "Plans Uploaded! The plans will be listed on the planned run page.", "failed": failed}
        return HttpResponse(json.dumps(r), mimetype="text/html")


@login_required
def get_application_product_presets(request):
    data = _get_allApplProduct_data(True)
    json_serializer = serializers.get_serializer("json")()
    fields = data.keys()
    result = json_serializer.serialize(data, fields=fields)
    return HttpResponse(result, mimetype="application/json")


def _get_allApplProduct_data(isForTemplate):
    def pretty(d, indent=0):
        if d:
            for key, value in d.iteritems():
                logger.debug('\t' * indent + str(key))
                if isinstance(value, dict):
                    pretty(value, indent + 1)
                else:
                    logger.debug('\t' * (indent + 1) + str(value))

    data = _get_base_planTemplate_data(isForTemplate)

    runTypes = list(RunType.objects.all())
    for appl in runTypes:

        applType = appl.runType

        try:
            #we should only have 1 default per application. TODO: add logic to ApplProduct to ensure that
            defaultApplProduct = list(ApplProduct.objects.filter(isActive=True, isDefault=True, applType=appl))
            if defaultApplProduct[0]:

                applData = {}

                applData["runType"] = defaultApplProduct[0].applType
                applData["reference"] = defaultApplProduct[0].defaultGenomeRefName
                applData["targetBedFile"] = defaultApplProduct[0].defaultTargetRegionBedFileName
                applData["hotSpotBedFile"] = defaultApplProduct[0].defaultHotSpotRegionBedFileName
                applData["seqKit"] = defaultApplProduct[0].defaultSequencingKit
                applData["libKit"] = defaultApplProduct[0].defaultLibraryKit
                applData["peSeqKit"] = defaultApplProduct[0].defaultPairedEndSequencingKit
                applData["peLibKit"] = defaultApplProduct[0].defaultPairedEndLibraryKit
                applData["chipType"] = defaultApplProduct[0].defaultChipType

                if defaultApplProduct[0].defaultChipType:
                    applData["chipTypeDetails"] = get_object_or_404(Chip, name=defaultApplProduct[0].defaultChipType)
                else:
                    applData["chipTypeDetails"] = None

                applData["isPairedEndSupported"] = defaultApplProduct[0].isPairedEndSupported
                applData["isDefaultPairedEnd"] = defaultApplProduct[0].isDefaultPairedEnd
                applData['defaultVariantFrequency'] = defaultApplProduct[0].defaultVariantFrequency

                applData['flowCount'] = defaultApplProduct[0].defaultFlowCount
                applData['peAdapterKit'] = defaultApplProduct[0].defaultPairedEndAdapterKit
                applData['defaultOneTouchTemplateKit'] = defaultApplProduct[0].defaultTemplateKit
                applData['controlSeqKit'] = defaultApplProduct[0].defaultControlSeqKit
                applData['isHotspotRegionBEDFileSupported'] = defaultApplProduct[0].isHotspotRegionBEDFileSuppported

                applData['isDefaultBarcoded'] = defaultApplProduct[0].isDefaultBarcoded
                applData['defaultBarcodeKitName'] = defaultApplProduct[0].defaultBarcodeKitName
                
                applData['defaultIonChefKit'] = defaultApplProduct[0].defaultIonChefPrepKit
                applData['defaultSamplePrepKit'] = defaultApplProduct[0].defaultSamplePrepKit
                
                #20120619-TODO-add compatible plugins, default plugins

                data[applType] = applData

#                if applType == 'AMPS':
#                    pretty(data[applType])
            else:
                data[applType] = 'none'
        except:
            data[applType] = 'none'

    return data


def _dict_IR_plugins_uploaders(isForTemplate):
    '''
    Returns a dict containing keys:
        irConfigSelection,
        irConfigSelection_1,
        plugins,
        uploaders,
    '''

    data = {}

    data['irConfigSelection_1'] = json.dumps(None)
    data['irConfigSelection'] = json.dumps(None)

    #based on features.EXPORT to determine if a plugin should be included in the Export tab
    uploaderNames = []
    pluginNames = []
    #selected=True + active=True = plugin ENABLED
    #since template creation/edit does not support IR configuration, we're going to skip going to the cloud to fetch IR
    #configuration selectable values just so to speed things up
    pluginCandidates = Plugin.objects.filter(selected=True, active=True)
    if isForTemplate:
        pluginCandidates = pluginCandidates.exclude(name__icontains="IonReporter")
        IRuploaders = list(Plugin.objects.filter(name__icontains="IonReporter", selected=True, active=True).order_by('name', '-version'))
    pluginCandidates = list(pluginCandidates.order_by('name', '-version'))

    # Issue bulk query for efficiency
    plugin_list = [(plugin.name, plugin.pluginscript(), {'plugin':plugin}) for plugin in pluginCandidates]
    from iondb.plugins.manager import pluginmanager
    pluginInfo = pluginmanager.get_plugininfo_list(plugin_list)

    # But don't use pluginInfo - query plugins individually via ORM
    for p in pluginCandidates:
        info = p.info()
        if info:
            infoName = info['name']
            if 'features' in info:
                #watch out: "Export" was changed to "export" recently!
                if ('export' in (feature.lower() for feature in info['features'])):
                    uploaderNames.append(infoName)
                else:
                    pluginNames.append(infoName)
            else:
                pluginNames.append(infoName)

            if infoName.lower() == 'IonReporterUploader_V1_0'.lower():
                if 'config' in info:
                    data['irConfigSelection_1'] = json.dumps(info['config'])
            elif ('IonReporterUploader'.lower() in infoName.lower()):
                if 'config' in info:
                    data['irConfigSelection'] = json.dumps(info['config'])

    #force querySet to be evaluated
    data['plugins'] = list(Plugin.objects.filter(selected=True, active=True).filter(name__in=pluginNames).order_by('name', '-version'))
    data['uploaders'] = list(Plugin.objects.filter(selected=True, active=True).filter(name__in=uploaderNames).order_by('name', '-version'))
    # add back IR plugins
    if isForTemplate:
        data['uploaders'] += IRuploaders
    return data


def _get_base_planTemplate_data(isForTemplate):
    data = {}

    #per requirement, we want to display Generic Sequencing as the last entry in the selection list
    data["runTypes"] = list(RunType.objects.all().exclude(runType = "GENS").order_by('nucleotideType', 'runType'))
    data["secondaryRunTypes"] = list(RunType.objects.filter(runType = "GENS"))
                
    data["barcodes"] = list(dnaBarcode.objects.values('name').distinct().order_by('name'))

    ##barcodeKitNames = dnaBarcode.objects.values_list('name', flat=True).distinct().order_by('name')
    ##for barcodeKitName in barcodeKitNames:
    ##    data[barcodeKitName] = dnaBarcode.objects.filter(name=barcodeKitName).order_by('index')

    data["barcodeKitInfo"] = list(dnaBarcode.objects.all().order_by('name', 'index'))

    references = list(ReferenceGenome.objects.all().filter(index_version=settings.TMAP_VERSION))
    data["references"] = references
    data["referenceShortNames"] = [ref.short_name for ref in references]

    data.update(dict_bed_hotspot())

    data["seqKits"] = KitInfo.objects.filter(kitType='SequencingKit', isActive=True).order_by("name")
    data["libKits"] = KitInfo.objects.filter(kitType='LibraryKit', isActive=True).order_by("name")

    data["variantfrequencies"] = VariantFrequencies.objects.all().order_by("name")

    #the entry marked as the default will be on top of the list
    data["forwardLibKeys"] = LibraryKey.objects.filter(direction='Forward', runMode='single').order_by('-isDefault', 'name')
    data["forward3Adapters"] = ThreePrimeadapter.objects.filter(direction='Forward', runMode='single').order_by('-isDefault', 'name')

    #pairedEnd does not have special forward library keys
    #for TS-4669: remove paired-end from wizard, if there are no active PE lib kits, do not prepare pe keys or adapters
    peLibKits = KitInfo.objects.filter(kitType='LibraryKit', runMode='pe', isActive=True)
    if (peLibKits.count() > 0):
        data["peForwardLibKeys"] = LibraryKey.objects.filter(direction='Forward').order_by('-isDefault', 'name')
        data["peForward3Adapters"] = ThreePrimeadapter.objects.filter(direction='Forward', runMode='pe').order_by('-isDefault', 'name')
        data["reverseLibKeys"] = LibraryKey.objects.filter(direction='Reverse').order_by('-isDefault', 'name')
        data["reverse3Adapters"] = ThreePrimeadapter.objects.filter(direction='Reverse').order_by('-isDefault', 'name')
    else:
        data["peForwardLibKeys"] = None
        data["peForward3Adapters"] = None
        data["reverseLibKeys"] = None
        data["reverse3Adapters"] = None

    #chip types
    #note: customer-facing chip names are no longer unique
    data['chipTypes'] = list(Chip.objects.filter(isActive=True).order_by('description', 'name').distinct('description'))
    #QC
    data['qcTypes'] = list(QCType.objects.all().order_by('qcName'))
    #project
    data['projects'] = list(Project.objects.filter(public=True).order_by('name'))

    #templating kit selection
    data["templateKits"] = KitInfo.objects.filter(kitType='TemplatingKit', isActive=True).order_by("name")
    #control sequence kit selection
    data["controlSeqKits"] = KitInfo.objects.filter(kitType='ControlSequenceKit', isActive=True).order_by("name")

    #ionChef kit selection
    data["ionChefKits"] = KitInfo.objects.filter(kitType='IonChefPrepKit', isActive=True).order_by("name")

    #pairedEnd library adapter selection
    #for TS-4669: remove paired-end from wizard, if there are no active PE seq kits, do not prepare pe keys or adapters
    if (peLibKits.count() > 0):
        data["pairedEndLibAdapters"] = KitInfo.objects.filter(kitType='AdapterKit', runMode="pe", isActive=True).order_by('name')
    else:
        data["pairedEndLibAdapters"] = None

    #samplePrep kits
    data["samplePrepKits"] = KitInfo.objects.filter(kitType='SamplePrepKit', isActive=True).order_by('name')

    data.update(_dict_IR_plugins_uploaders(isForTemplate))

    #to allow data entry for multiple non-barcoded samples at the plan wizard
    data['nonBarcodedSamples_irConfig_loopCounter'] = [i + 1 for i in range(20)]

    return data


@login_required
@transaction.commit_manually
def save_plan_or_template(request, planOid):
    """
    Saving new or edited plan/template to db (source: plan template wizard)
    Editing a planned run from having 1 sample to 2 samples will result in one edited planned run and one new planned run
    """
    def isReusable(submitIntent):
        return not (submitIntent == 'savePlan' or submitIntent == 'updatePlan')


    if request.method != 'POST':
        logger.exception(format_exc())
        return HttpResponse(json.dumps({"error": "Error, unsupported HTTP Request method (%s) for plan update." % request.method}), mimetype="application/json")

    # Process Inputs

    # pylint:disable=E1103
    json_data = simplejson.loads(request.raw_post_data)
    submitIntent = json_data.get('submitIntent', '')
    logger.debug('views.save_plan_or_template POST.raw_post_data... simplejson Data: "%s"' % json_data)
    logger.debug("views.save_plan_or_template submitIntent=%s" % submitIntent)
    # saving Template or Planned Run
    isReusable = isReusable(submitIntent)
    runModeValue = json_data.get('runMode', 'single')
    isPlanGroupValue = runModeValue == 'pe' and not isReusable
    libraryKeyValue = json_data.get('libraryKey', '')
    forward3primeAdapterValue = json_data.get('forward3primeAdapter', '')

    msgvalue = 'Run Plan' if not isReusable else 'Template'
    if runModeValue == 'pe':
        return HttpResponse(json.dumps({"error": "Error, paired-end plan is no longer supported. %s will not be saved." % (msgvalue)}), mimetype="application/html")
    
    planDisplayedNameValue = json_data.get('planDisplayedName', '').strip()
    noteValue = json_data.get('notes_workaround', '')

    # perform server-side validation to avoid things falling through the crack    
    if not planDisplayedNameValue:
        return HttpResponse(json.dumps({"error": "Error, please enter a %s Name."  %(msgvalue)}), mimetype="application/html")

    if not is_valid_chars(planDisplayedNameValue):
        return HttpResponse(json.dumps({"error": "Error, %s Name" %(msgvalue) + ERROR_MSG_INVALID_CHARS}), mimetype="application/html")        
        
    if not is_valid_length(planDisplayedNameValue, MAX_LENGTH_PLAN_NAME):
        return HttpResponse(json.dumps({"error": "Error, %s Name"  %(msgvalue) + ERROR_MSG_INVALID_LENGTH  %(str(MAX_LENGTH_PLAN_NAME))}), mimetype="application/html")

    if noteValue:
        if not is_valid_chars(noteValue):
            return HttpResponse(json.dumps({"error": "Error, %s note" %(msgvalue) + ERROR_MSG_INVALID_CHARS}), mimetype="application/html")
        
        if not is_valid_length(noteValue, MAX_LENGTH_NOTES):
            return HttpResponse(json.dumps({"error": "Error, Note" + ERROR_MSG_INVALID_LENGTH  %(str(MAX_LENGTH_NOTES))}), mimetype="application/html")

    # Projects
    projectObjList = get_projects(request.user, json_data)

    # IonReporterUploader configuration and samples
    selectedPlugins = json_data.get('selectedPlugins', {})
    IRconfigList = json_data.get('irConfigList', [])

    IRU_selected = False
    for uploader in selectedPlugins.values():
        if 'ionreporteruploader' in uploader['name'].lower() and uploader['name'] != 'IonReporterUploader_V1_0':
            IRU_selected = True

    #if IRU is set to autoRun, user does not need to select the plugin explicitly. user could have set all IRU versions to autorun
    IRU_autorun_count = 0
    if not IRU_selected:
        IRU_autoruns = Plugin.objects.filter(name__icontains="IonReporter", selected=True, active=True, autorun=True).exclude(name__icontains="IonReporterUploader_V1_0").order_by('-name')
        IRU_autorun_count = IRU_autoruns.count()
        if IRU_autorun_count > 0:
            IRU_selected = True
    
    if IRU_selected:
        samples_IRconfig = json_data.get('sample_irConfig', '')

        if samples_IRconfig:
            samples_IRconfig = ','.join(samples_IRconfig)

        #generate UUID for unique setIds
        id_uuid = {}
        setids = [ir.get('setid', "") for ir in IRconfigList]

        if setids:
            for setid in set(setids):
                if setid:                    
                    id_uuid[setid] = str(uuid.uuid4())
            for ir_config in IRconfigList:
                setid = ir_config.get('setid', '')
                                
                if setid:
                    ir_config['setid'] += '__' + id_uuid[setid]

        if IRU_autorun_count > 0 and not samples_IRconfig:
            #if more than one IRU version is set to autorun and user does not explicitly select one, 
            #gui shows workflow config for IRU v1.0
            samples_IRconfig = json_data.get('samples_workaround', '')
        
    # Samples
    barcodeIdValue = json_data.get('barcodeId', '')
    barcodedSamples = ''
    sampleValidationErrorMsg = ''
    sampleValidationErrorMsg_leadingChars = ''
    sampleValidationErrorMsg_length = ''
        
    # one Plan will be created per entry in sampleList
    # samples for barcoded Plan have a separate field (barcodedSamples)

    if isReusable:
        # samples entered only when saving planned run (not template)
        sampleList = ['']
    elif barcodeIdValue:
        # a barcode Set is selected
        sampleList = ['']
        bcSamplesValues = json_data.get('bcSamples_workaround', '')
        bcDictionary = {}
        bcId = ""
        for token in bcSamplesValues.split(","):
            if ((token.find("bcKey|")) == 0):
                bcId, bcId_str = token.split("|")[1:]
            else:
                sample = token.strip()
                if bcId and sample:
                    if not is_valid_chars(sample):
                        sampleValidationErrorMsg += sample + ', '
                    elif is_invalid_leading_chars(sample):
                        sampleValidationErrorMsg_leadingChars += sample + ", "
                    elif not is_valid_length(sample, MAX_LENGTH_SAMPLE_NAME):
                        sampleValidationErrorMsg_length += sample + ", "
                        
                    bcDictionary.setdefault(sample, {}).setdefault('barcodes',[]).append(bcId_str)
                bcId = ""

        barcodedSamples = simplejson.dumps(bcDictionary)
        logger.debug("views.save_plan_or_template after simplejson.dumps... barcodedSamples=%s;" % (barcodedSamples))

        if not bcDictionary:
            transaction.rollback()
            return HttpResponse(json.dumps({"error": "Error, please enter at least one barcode sample name."}), mimetype="application/html")

    else:
        # Non-barcoded samples
        sampleList = []
        if IRU_selected:
            samples = samples_IRconfig
        else:
            samples = json_data.get('samples_workaround', '')

        for sample in samples.split(','):
            if sample.strip():
                if not is_valid_chars(sample):
                    sampleValidationErrorMsg += sample + ', '
                elif is_invalid_leading_chars(sample):
                    sampleValidationErrorMsg_leadingChars += sample + ", "
                elif not is_valid_length(sample, MAX_LENGTH_SAMPLE_NAME):
                    sampleValidationErrorMsg_length += sample + ", "
                else:
                    sampleList.append(sample)

        logger.debug("views.save_plan_or_template sampleList=%s " % (sampleList))

        if  len(sampleList) == 0 and not sampleValidationErrorMsg and not sampleValidationErrorMsg_leadingChars and not sampleValidationErrorMsg_length:
            transaction.rollback()
            return HttpResponse(json.dumps({"error": "Error, please enter a sample name for the run plan."}), mimetype="application/html")
    
    # Samples validation
    if sampleValidationErrorMsg or sampleValidationErrorMsg_leadingChars or sampleValidationErrorMsg_length:
        message = ""
        if sampleValidationErrorMsg:
            message = "Error, sample name" + ERROR_MSG_INVALID_CHARS
            message = message + ' <br>Please fix: ' + sampleValidationErrorMsg + '<br>'
        if sampleValidationErrorMsg_leadingChars:
            message = message + "Error, sample name" + ERROR_MSG_INVALID_LEADING_CHARS
            message = message + ' <br>Please fix: ' + sampleValidationErrorMsg_leadingChars + '<br>'
        if sampleValidationErrorMsg_length:
            message = message + "Error, sample name" + ERROR_MSG_INVALID_LENGTH  %(str(MAX_LENGTH_SAMPLE_NAME))
            message = message + ' <br>Please fix: ' + sampleValidationErrorMsg_length
          
        transaction.rollback()
        return HttpResponse(json.dumps({"error": message}), mimetype="application/html")

    selectedPluginsValue = json_data.get('selectedPlugins', {})

    # end processing input data

    # Edit/Create Plan(s)

    if int(planOid) == 0:
        edit_existing_plan = False
    else:
        edit_existing_plan = True

    for i, sample in enumerate(sampleList):
        logger.debug("...LOOP... views.save_plan_or_template SAMPLE=%s; isSystem=%s; isReusable=%s; isPlanGroup=%s "
                     % (sample.strip(), json_data["isSystem"], isReusable, isPlanGroupValue))

        # add IonReporter config values for each sample
        if len(IRconfigList) > 0:
            for uploader in selectedPluginsValue.values():
                if 'ionreporteruploader' in uploader['name'].lower():
                    if len(IRconfigList) > 1 and not barcodeIdValue:
                        uploader['userInput'] = [IRconfigList[i]]
                    else:
                        uploader['userInput'] = IRconfigList

        if len(sampleList) > 1:
            inputPlanDisplayedName = planDisplayedNameValue + '_' + sample.strip()
        else:
            inputPlanDisplayedName = planDisplayedNameValue
            
        selectedTemplatingKit = json_data.get('templatekitname', '')
        samplePrepInstrumentType = json_data.get('samplePrepInstrumentType', '')
        if samplePrepInstrumentType == 'ionChef':
            selectedTemplatingKit = json_data.get('templatekitionchefname', '')
        
        #PDD-TODO: remove the x_ prefix. the x_ prefix is just a reminder what the obsolete attributes to remove during the next phase
        kwargs = {
            'planDisplayedName': inputPlanDisplayedName,
            "planName": inputPlanDisplayedName.replace(' ', '_'),
            'usePreBeadfind': toBoolean(json_data['usePreBeadfind'], False),
            'usePostBeadfind': toBoolean(json_data['usePostBeadfind'], False),
            'preAnalysis': True,
            'runType': json_data['runType'],
            'templatingKitName': selectedTemplatingKit,
            'controlSequencekitname': json_data.get('controlsequence', ''),
            'runMode': runModeValue,
            'isSystem': toBoolean(json_data['isSystem'], False),
            'isReusable': isReusable,
            'isPlanGroup': isPlanGroupValue,
            'username': request.user.username,
            'isFavorite': toBoolean(json_data.get('isFavorite', 'False'), False),
            'pairedEndLibraryAdapterName': json_data.get('pairedEndLibraryAdapterName', ''),
            'samplePrepKitName': json_data.get('samplePrepKitName', ''),
            'planStatus' : "planned",

            'x_autoAnalyze': True,
            'x_barcodedSamples': barcodedSamples,
            'x_barcodeId': barcodeIdValue,
            'x_bedfile': json_data.get('bedfile', ''),
            'x_chipType': json_data.get('chipType', ''),
            'x_flows': json_data.get('flows', None),
            'x_forward3primeadapter': forward3primeAdapterValue,
            ###'_isReverseRun':  = self.isReverseRun
            'x_library': json_data.get('library', ''),
            'x_libraryKey': libraryKeyValue,
            'x_librarykitname': json_data.get('librarykitname', ''),
            'x_notes': noteValue,
            'x_regionfile': json_data.get('regionfile', ''),
            'x_sample': sample.strip().replace(' ', '_'),
            'x_sampleDisplayedName': sample.strip(),
            'x_selectedPlugins': selectedPluginsValue,
            'x_sequencekitname': json_data.get('sequencekitname', ''),
            'x_variantfrequency': json_data.get('variantfrequency', ''),
        }

        planTemplate = None
        
        #if we're changing a plan from having 1 sample to say 2 samples, we need to UPDATE 1 plan and CREATE 1 plan!!
        try:
            if not edit_existing_plan:
                planTemplate, extra_kwargs = PlannedExperiment.objects.save_plan(-1, **kwargs)             
            else:
                planTemplate, extra_kwargs = PlannedExperiment.objects.save_plan(planOid, **kwargs)
                
                edit_existing_plan = False

            # Update QCtype thresholds
            qcTypes = QCType.objects.all()
            for qcType in qcTypes:
                qc_threshold = json_data.get(qcType.qcName, '')
                if qc_threshold:
                    # get existing PlannedExperimentQC if any
                    plannedExpQcs = PlannedExperimentQC.objects.filter(plannedExperiment=planTemplate.id, qcType=qcType.id)
                    if len(plannedExpQcs) > 0:
                        for plannedExpQc in plannedExpQcs:
                            plannedExpQc.threshold = qc_threshold
                            plannedExpQc.save()
                    else:
                        kwargs = {
                            'plannedExperiment': planTemplate,
                            'qcType': qcType,
                            'threshold': qc_threshold
                        }
                        plannedExpQc = PlannedExperimentQC(**kwargs)
                        plannedExpQc.save()

            # add/remove projects
            if projectObjList:
                #TODO: refactor this logic to simplify using django orm
                projectNameList = [project.name for project in projectObjList]
                for currentProject in planTemplate.projects.all():
                    if currentProject.name not in projectNameList:
                        planTemplate.projects.remove(currentProject)
                for projectObj in projectObjList:
                    planTemplate.projects.add(projectObj)
            else:
                planTemplate.projects.clear()
                
        except ValidationError, err:
            transaction.rollback()
            logger.exception(format_exc())
            
            message = "Internal error while trying to save the plan. "
            for msg in err.messages:                
                message += str(msg)
                message += " "

            return HttpResponse(json.dumps({"error": message}), mimetype="application/json")

        except Exception as excp:
            transaction.rollback()
            logger.exception(format_exc())

            message = "Internal error while trying to save the plan. %s" %(excp.message)
            return HttpResponse(json.dumps({"error": message}), mimetype="application/json")
        except:
            transaction.rollback()
            logger.exception(format_exc())
            return HttpResponse(json.dumps({"error": "Internal error while trying to save the plan."}), mimetype="application/json")
        else:
            transaction.commit()

    return HttpResponse(json.dumps({"status": "plan template updated successfully"}), mimetype="application/json")
